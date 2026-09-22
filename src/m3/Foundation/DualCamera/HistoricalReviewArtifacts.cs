using System.Security.Cryptography;
using System.Text.Json;

namespace A0CameraStitcher.M3.Foundation.DualCamera;

/// <summary>Read-only evidence for reopening a pending historical review.</summary>
public sealed record HistoricalReviewArtifacts(
    Guid JobId,
    Guid TransactionId,
    string ReviewKind,
    IReadOnlyList<CanonicalJpegOriginal> Originals,
    string StitchedPath,
    string StitchedSha256,
    int Width,
    int Height,
    string ProfileId,
    string ProfileVersion);

internal static class HistoricalReviewArtifactsVerifier
{
    private const long MaximumBytes = 64L * 1024L * 1024L;

    internal static async Task<HistoricalReviewArtifacts> VerifyAsync(
        M2OfflineStitcherProcessAdapter adapter, string productRoot, OperatorReviewRecord record,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(adapter);
        ArgumentNullException.ThrowIfNull(record);
        cancellationToken.ThrowIfCancellationRequested();
        if (record.State != "Pending" || record.ReviewKind is not ("Product" or "Simulated") ||
            !Guid.TryParseExact(record.ResultId, "N", out var jobId) || jobId == Guid.Empty ||
            !Guid.TryParseExact(record.TransactionId, "N", out var transactionId) || transactionId == Guid.Empty)
            throw new InvalidDataException("Historical review identity is invalid.");

        var root = ValidateFixedLocalRoot(productRoot);
        var jobDirectory = CanonicalUnder(root, "stitch-jobs", jobId.ToString("N"));
        var stitchedPath = CanonicalUnder(jobDirectory, "stitched.jpg");
        var manifestPath = CanonicalUnder(jobDirectory, "stitch-job.manifest.json");
        var originalPaths = new[]
        {
            (Alias: "CAM-A", Path: CanonicalUnder(root, "transactions", transactionId.ToString("N"), "CAM-A", "original.jpg")),
            (Alias: "CAM-B", Path: CanonicalUnder(root, "transactions", transactionId.ToString("N"), "CAM-B", "original.jpg")),
        };
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(30));
        var token = timeout.Token;
        token.ThrowIfCancellationRequested();

        // ShareRead holds a read lock against writers while the adapter and the
        // managed hash/header checks observe the exact immutable candidates.
        using var stitched = OpenRegularReadLocked(stitchedPath);
        using var manifest = OpenRegularReadLocked(manifestPath);
        var lockedOriginals = new List<(string Alias, string Path, FileStream Stream)>(2);
        try
        {
            foreach (var item in originalPaths)
            {
                token.ThrowIfCancellationRequested();
                lockedOriginals.Add((item.Alias, item.Path, OpenRegularReadLocked(item.Path)));
            }
            token.ThrowIfCancellationRequested();
            var output = await adapter.RunAsync(
                ["verify-published-stitch", "--job-directory", jobDirectory, "--stitch-job-id", jobId.ToString("N"),
                 "--capture-transaction-id", transactionId.ToString("N")], token).ConfigureAwait(false);
            var metadata = ParseNativeVerification(output, jobId, transactionId);
            var originals = new List<CanonicalJpegOriginal>(2);
            foreach (var item in lockedOriginals)
            {
                var expected = metadata.Inputs.SingleOrDefault(input => input.Alias == item.Alias);
                if (expected is null) throw new InvalidDataException("Manifest input aliases are invalid.");
                var bytes = await ReadLockedAsync(item.Stream, token).ConfigureAwait(false);
                if (bytes.LongLength != expected.Size || Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant() != expected.Sha256)
                    throw new InvalidDataException("Historical original hash does not match the manifest.");
                RequireJpegEnvelope(bytes);
                var (width, height) = DualCameraProductFlow.ReadJpegDimensions(bytes);
                await adapter.ValidateCanonicalJpegAsync(item.Path, width, height, token).ConfigureAwait(false);
                originals.Add(new CanonicalJpegOriginal(item.Alias, item.Path, bytes.LongLength, expected.Sha256, width, height, true));
            }
            if (originals.Count != 2 || originals[0].Alias != "CAM-A" || originals[1].Alias != "CAM-B")
                throw new InvalidDataException("Historical originals are incomplete.");
            var stitchedBytes = await ReadLockedAsync(stitched, token).ConfigureAwait(false);
            if (stitchedBytes.LongLength != metadata.OutputSize ||
                Convert.ToHexString(SHA256.HashData(stitchedBytes)).ToLowerInvariant() != metadata.OutputSha256)
                throw new InvalidDataException("Historical stitched output changed during verification.");
            RequireJpegEnvelope(stitchedBytes);
            var (outputWidth, outputHeight) = DualCameraProductFlow.ReadJpegDimensions(stitchedBytes);
            if (outputWidth != metadata.Width || outputHeight != metadata.Height)
                throw new InvalidDataException("Historical stitched dimensions do not match the manifest.");
            await adapter.ValidateCanonicalJpegAsync(stitchedPath, outputWidth, outputHeight, token).ConfigureAwait(false);
            return new HistoricalReviewArtifacts(jobId, transactionId, record.ReviewKind, originals, stitchedPath,
                metadata.OutputSha256, outputWidth, outputHeight, metadata.ProfileId, metadata.ProfileVersion);
        }
        finally
        {
            foreach (var item in lockedOriginals) item.Stream.Dispose();
        }
    }

    private static string ValidateFixedLocalRoot(string value)
    {
        if (string.IsNullOrWhiteSpace(value) || !Path.IsPathFullyQualified(value)) throw new ArgumentException("Product root is invalid.");
        var root = Path.GetFullPath(value);
        if (!Directory.Exists(root) || root.StartsWith("\\\\", StringComparison.Ordinal) || root.StartsWith("//", StringComparison.Ordinal) ||
            root.StartsWith("\\\\?\\", StringComparison.Ordinal) || root.StartsWith("\\\\.\\", StringComparison.Ordinal))
            throw new ArgumentException("Product root is unavailable.");
        var drive = Path.GetPathRoot(root);
        if (string.IsNullOrWhiteSpace(drive) || new DriveInfo(drive).DriveType != DriveType.Fixed) throw new ArgumentException("Product root is not local.");
        RejectReparseAncestors(root);
        return root;
    }

    private static string CanonicalUnder(string root, params string[] segments)
    {
        var candidate = Path.GetFullPath(Path.Combine(new[] { root }.Concat(segments).ToArray()));
        if (!candidate.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + Path.DirectorySeparatorChar,
                                  StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("Historical path escaped its product root.");
        return candidate;
    }

    private static FileStream OpenRegularReadLocked(string path)
    {
        RejectReparseAncestors(Path.GetDirectoryName(path) ?? throw new InvalidDataException("Historical path has no parent."));
        if (!File.Exists(path) || (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new FileNotFoundException();
        var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 81920, FileOptions.Asynchronous | FileOptions.SequentialScan);
        if (stream.Length is <= 0 or > MaximumBytes) { stream.Dispose(); throw new InvalidDataException("Historical file size is invalid."); }
        return stream;
    }

    private static async Task<byte[]> ReadLockedAsync(FileStream stream, CancellationToken token)
    {
        if (stream.Length is <= 0 or > MaximumBytes) throw new InvalidDataException("Historical file size is invalid.");
        stream.Position = 0;
        var bytes = new byte[checked((int)stream.Length)];
        var offset = 0;
        while (offset < bytes.Length)
        {
            var read = await stream.ReadAsync(bytes.AsMemory(offset), token).ConfigureAwait(false);
            if (read == 0) throw new InvalidDataException("Historical file changed during read.");
            offset += read;
        }
        if (stream.Length != bytes.Length) throw new InvalidDataException("Historical file changed during read.");
        return bytes;
    }

    private static void RejectReparseAncestors(string directory)
    {
        for (DirectoryInfo? current = new(directory); current is not null; current = current.Parent)
        {
            if (!current.Exists || (current.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Historical path is unavailable or redirected.");
        }
    }

    private sealed record Input(string Alias, string Sha256, long Size);
    private sealed record Metadata(IReadOnlyList<Input> Inputs, string OutputSha256, long OutputSize, int Width, int Height,
                                   string ProfileId, string ProfileVersion);

    private static Metadata ParseNativeVerification(string output, Guid job, Guid transaction)
    {
        var lines = output.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries);
        if (lines.Length != 2 || lines[0] != "result=verified-published-stitch" || !lines[1].StartsWith("manifestJson=", StringComparison.Ordinal))
            throw new InvalidDataException("Native verification response is invalid.");
        using var document = JsonDocument.Parse(lines[1]["manifestJson=".Length..]);
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object || !Exact(root,
                root.TryGetProperty("schemaVersion", out var schema) && schema.ValueKind == JsonValueKind.String && schema.GetString() == "a0.stitch-job-manifest.v2"
                    ? ["schemaVersion", "stitchJobId", "captureTransactionId", "inputs", "rigProfile", "engine", "output", "seamNavigation", "terminalResultState", "completedAtUtc", "automaticRetryCount"]
                    : ["schemaVersion", "stitchJobId", "captureTransactionId", "inputs", "rigProfile", "engine", "output", "terminalResultState", "completedAtUtc", "automaticRetryCount"]) ||
            !String(root, "stitchJobId", job.ToString("N")) ||
            !String(root, "captureTransactionId", transaction.ToString("N")) ||
            !String(root, "terminalResultState", "Succeeded") || !String(root, "schemaVersion", "a0.stitch-job-manifest.v1", "a0.stitch-job-manifest.v2"))
            throw new InvalidDataException("Manifest identity is invalid.");
        var inputs = root.GetProperty("inputs");
        if (inputs.ValueKind != JsonValueKind.Array || inputs.GetArrayLength() != 2) throw new InvalidDataException("Manifest inputs are invalid.");
        var parsedInputs = inputs.EnumerateArray().Select(input =>
        {
            if (input.ValueKind != JsonValueKind.Object || !Exact(input, "cameraAlias", "sha256", "encodedSizeBytes"))
                throw new InvalidDataException("Manifest input fields are invalid.");
            return new Input(RequiredString(input, "cameraAlias"), RequiredSha(input, "sha256"), RequiredLong(input, "encodedSizeBytes"));
        }).ToArray();
        if (parsedInputs.Select(input => input.Alias).Distinct(StringComparer.Ordinal).Count() != 2 ||
            !parsedInputs.Any(input => input.Alias == "CAM-A") || !parsedInputs.Any(input => input.Alias == "CAM-B"))
            throw new InvalidDataException("Manifest aliases are invalid.");
        var profile = root.GetProperty("rigProfile");
        var outputElement = root.GetProperty("output");
        if (profile.ValueKind != JsonValueKind.Object || !Exact(profile, "profileId", "version", "sha256") ||
            outputElement.ValueKind != JsonValueKind.Object || !Exact(outputElement, "relativePath", "sha256", "widthPixels", "heightPixels", "encodedSizeBytes") ||
            !String(outputElement, "relativePath", "stitched.jpg"))
            throw new InvalidDataException("Manifest output path is invalid.");
        return new Metadata(parsedInputs, RequiredSha(outputElement, "sha256"), RequiredLong(outputElement, "encodedSizeBytes"),
            RequiredInt(outputElement, "widthPixels"), RequiredInt(outputElement, "heightPixels"),
            RequiredString(profile, "profileId"), RequiredString(profile, "version"));
    }

    private static bool Exact(JsonElement element, params string[] names)
    {
        var properties = element.EnumerateObject().Select(property => property.Name).ToArray();
        return properties.Length == names.Length && properties.ToHashSet(StringComparer.Ordinal).SetEquals(names);
    }
    private static bool String(JsonElement element, string name, params string[] values) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String && value.GetString() is { } text && values.Contains(text, StringComparer.Ordinal);
    private static string RequiredString(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String && !string.IsNullOrEmpty(value.GetString()) ? value.GetString()! : throw new InvalidDataException("Manifest string is invalid.");
    private static string RequiredSha(JsonElement element, string name)
    {
        var value = RequiredString(element, name);
        return value.Length == 64 && value.All(character => character is >= '0' and <= '9' || character is >= 'a' and <= 'f') ? value : throw new InvalidDataException("Manifest hash is invalid.");
    }
    private static long RequiredLong(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.TryGetInt64(out var number) && number is > 0 and <= MaximumBytes ? number : throw new InvalidDataException("Manifest size is invalid.");
    private static int RequiredInt(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.TryGetInt32(out var number) && number > 0 ? number : throw new InvalidDataException("Manifest dimensions are invalid.");
    private static void RequireJpegEnvelope(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length < 4 || bytes[0] != 0xff || bytes[1] != 0xd8 || bytes[^2] != 0xff || bytes[^1] != 0xd9)
            throw new InvalidDataException("Historical JPEG envelope is invalid.");
    }
}
