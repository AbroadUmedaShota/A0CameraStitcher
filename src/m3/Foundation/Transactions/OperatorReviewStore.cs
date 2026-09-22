using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace A0CameraStitcher.M3.Foundation;

public sealed record OperatorReviewRecord
{
    public required string ResultId { get; init; }
    public required string TransactionId { get; init; }
    public required string ReviewKind { get; init; }
    public required string State { get; init; }
    public required DateTimeOffset UpdatedAtUtc { get; init; }
}

public interface IOperatorReviewStore
{
    Task SaveAsync(OperatorReviewRecord record, CancellationToken cancellationToken = default);
    Task<IReadOnlyList<OperatorReviewRecord>> LoadAsync(CancellationToken cancellationToken = default);
}

public sealed record ReviewMetadataPage(IReadOnlyList<OperatorReviewRecord> Records, int? NextOffset, int Total);

// Review metadata is separate from immutable capture originals and product manifests.
// The caller must verify the selected result before recording human acceptance.
public sealed class FileOperatorReviewStore(string rootDirectory) : IOperatorReviewStore
{
    private readonly string _root = Path.GetFullPath(rootDirectory);
    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        WriteIndented = true,
    };

    public async Task SaveAsync(OperatorReviewRecord record, CancellationToken cancellationToken = default)
    {
        Validate(record);
        using var lease = AcquireLease();
        var path = RecordPath(record.ResultId);
        if (File.Exists(path))
        {
            var previous = await ReadAsync(path, cancellationToken);
            if (previous.ResultId != record.ResultId || previous.TransactionId != record.TransactionId ||
                previous.ReviewKind != record.ReviewKind)
                throw new InvalidDataException("Review target cannot be replaced.");
            if (previous == record) return;
            if (previous.State == "Accepted" || record.UpdatedAtUtc < previous.UpdatedAtUtc)
                throw new InvalidDataException("Review state cannot move backwards or change after acceptance.");
        }
        else if (record.State != "Pending")
        {
            throw new InvalidDataException("A result must be recorded pending before acceptance.");
        }

        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".partial";
        // Preserve incomplete metadata for diagnosis, never delete original images.
        await using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write,
                         FileShare.None, 4096, FileOptions.WriteThrough | FileOptions.Asynchronous))
        {
            await JsonSerializer.SerializeAsync(stream, record, Options, cancellationToken);
            await stream.FlushAsync(cancellationToken);
            stream.Flush(flushToDisk: true);
        }
        cancellationToken.ThrowIfCancellationRequested();
        File.Move(temporary, path, overwrite: true);
        // Once published, caller cancellation must not turn a committed acceptance
        // into a cancelled response. Finish verifying the durable result instead.
        if (await ReadAsync(path, CancellationToken.None) != record)
            throw new IOException("Review record reread verification failed.");
    }

    public async Task<IReadOnlyList<OperatorReviewRecord>> LoadAsync(CancellationToken cancellationToken = default)
    {
        using var lease = AcquireLease();
        if (Directory.EnumerateFiles(_root, "*.partial").Any())
            throw new InvalidDataException("Incomplete review metadata requires inspection; no review state was inferred.");
        var records = new List<OperatorReviewRecord>();
        foreach (var path in Directory.EnumerateFiles(_root, "*.json"))
        {
            var record = await ReadAsync(path, cancellationToken);
            if (!string.Equals(path, RecordPath(record.ResultId), StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Review record filename does not match its result.");
            records.Add(record);
        }
        return records.OrderBy(record => record.UpdatedAtUtc).ToArray();
    }

    // Internal acceptance transition for an already displayed Pending record.
    // It never creates metadata storage or a lock; callers must explicitly
    // confirm the review after independently re-verifying the artifacts.
    internal async Task<OperatorReviewRecord> AcceptPendingAsync(OperatorReviewRecord expected,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(expected);
        Validate(expected);
        if (expected.State != "Pending") throw new InvalidDataException("Only Pending reviews can be accepted.");
        using var lease = AcquireExistingLease();
        if (Directory.EnumerateFiles(_root, "*.partial").Any())
            throw new InvalidDataException("Incomplete review metadata requires inspection.");
        var path = RecordPath(expected.ResultId);
        if (!File.Exists(path)) throw new FileNotFoundException("Expected Pending review is unavailable.");
        var current = await ReadAsync(path, cancellationToken).ConfigureAwait(false);
        if (!Same(current, expected) || current.State != "Pending")
            throw new InvalidDataException("Pending review changed before acceptance.");
        var acceptedAt = DateTimeOffset.UtcNow;
        if (acceptedAt < expected.UpdatedAtUtc) throw new InvalidDataException("Review clock moved backwards.");
        var accepted = expected with { State = "Accepted", UpdatedAtUtc = acceptedAt };
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".partial";
        await using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write,
                         FileShare.None, 4096, FileOptions.WriteThrough | FileOptions.Asynchronous))
        {
            await JsonSerializer.SerializeAsync(stream, accepted, Options, cancellationToken).ConfigureAwait(false);
            await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
            stream.Flush(flushToDisk: true);
        }
        cancellationToken.ThrowIfCancellationRequested();
        File.Move(temporary, path, overwrite: true);
        var reread = await ReadAsync(path, CancellationToken.None).ConfigureAwait(false);
        if (!Same(reread, accepted)) throw new IOException("Accepted review reread verification failed.");
        return reread;
    }

    // Standalone metadata observation only: never initializes storage, accepts a
    // review, verifies image quality, or starts/queries the Camera Agent.
    public async Task<ReviewMetadataPage> QueryReadOnlyAsync(int offset = 0, int limit = 25,
        CancellationToken cancellationToken = default)
    {
        if (offset is < 0 or > 1000 || limit is < 1 or > 25)
            throw new ArgumentOutOfRangeException(nameof(offset), "Review query bounds exceeded.");
        cancellationToken.ThrowIfCancellationRequested();
        for (DirectoryInfo? ancestor = new(_root); ancestor is not null; ancestor = ancestor.Parent)
        {
            if (!ancestor.Exists) throw new DirectoryNotFoundException("Review storage is unavailable.");
            if ((ancestor.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Review storage ancestors must not be reparse points.");
        }
        var lockPath = Path.Combine(_root, ".review.lock");
        RejectReparsePoint(lockPath);
        // Existing GUI writers use FileShare.None too. Opening an existing lock
        // for reading prevents an inconsistent observation without creating it.
        using var lease = new FileStream(lockPath, FileMode.Open, FileAccess.Read, FileShare.None);
        if (Directory.EnumerateFiles(_root, "*.partial").Any())
            throw new InvalidDataException("Incomplete review metadata requires inspection.");
        var records = new List<OperatorReviewRecord>();
        foreach (var path in Directory.EnumerateFiles(_root, "*.json"))
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (records.Count == 1000) throw new InvalidDataException("Review query scan limit exceeded.");
            var record = await ReadAsync(path, cancellationToken);
            if (!string.Equals(path, RecordPath(record.ResultId), StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Review filename does not match its result.");
            records.Add(record);
        }
        var page = records.OrderByDescending(record => record.UpdatedAtUtc)
            .ThenBy(record => record.ResultId, StringComparer.Ordinal).Skip(offset).Take(limit).ToArray();
        int? next = offset + page.Length < records.Count ? offset + page.Length : null;
        return new ReviewMetadataPage(page, next, records.Count);
    }

    private FileStream AcquireLease()
    {
        Directory.CreateDirectory(_root);
        if ((File.GetAttributes(_root) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("Review directory must not be a reparse point.");
        var lockPath = Path.Combine(_root, ".review.lock");
        RejectReparsePoint(lockPath);
        return new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
    }

    private FileStream AcquireExistingLease()
    {
        for (DirectoryInfo? ancestor = new(_root); ancestor is not null; ancestor = ancestor.Parent)
        {
            if (!ancestor.Exists) throw new DirectoryNotFoundException("Review storage is unavailable.");
            if ((ancestor.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Review storage ancestors must not be reparse points.");
        }
        var lockPath = Path.Combine(_root, ".review.lock");
        RejectReparsePoint(lockPath);
        return new FileStream(lockPath, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
    }

    private string RecordPath(string id) => Path.Combine(_root,
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(id))).ToLowerInvariant() + ".json");

    private static async Task<OperatorReviewRecord> ReadAsync(string path, CancellationToken cancellationToken)
    {
        RejectReparsePoint(path);
        await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (stream.Length > 16384) throw new InvalidDataException("Review record exceeds size limit.");
        var record = await JsonSerializer.DeserializeAsync<OperatorReviewRecord>(stream, Options, cancellationToken)
            ?? throw new InvalidDataException("Review record is empty.");
        Validate(record);
        return record;
    }

    private static void RejectReparsePoint(string path)
    {
        if (File.Exists(path) && (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("Review file must not be a reparse point.");
    }

    private static void Validate(OperatorReviewRecord record)
    {
        ArgumentNullException.ThrowIfNull(record);
        if (string.IsNullOrWhiteSpace(record.ResultId) || record.ResultId.Length > 256 ||
            string.IsNullOrWhiteSpace(record.TransactionId) || record.TransactionId.Length > 256 ||
            record.ReviewKind is not ("Product" or "OriginalsOnly" or "Simulated") ||
            record.State is not ("Pending" or "Accepted") || record.UpdatedAtUtc == default)
            throw new InvalidDataException("Review record is invalid.");
    }

    private static bool Same(OperatorReviewRecord left, OperatorReviewRecord right) =>
        left.ResultId == right.ResultId && left.TransactionId == right.TransactionId &&
        left.ReviewKind == right.ReviewKind && left.State == right.State && left.UpdatedAtUtc == right.UpdatedAtUtc;
}
