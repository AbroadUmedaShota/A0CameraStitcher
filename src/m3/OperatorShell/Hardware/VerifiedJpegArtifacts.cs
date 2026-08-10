using System.IO;
using System.Security.Cryptography;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

public sealed record VerifiedHardwareJpeg(
    string Path,
    long SizeBytes,
    string Sha256);

public static class HardwareArtifactVerifier
{
    public static Task<VerifiedHardwareJpeg> VerifyOriginalAsync(
        HardwareRetainedOriginalRecord original,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(original);
        if (original.CameraAlias is not ("CAM-A" or "CAM-B"))
        {
            throw new InvalidDataException("Retained original alias is invalid.");
        }

        return VerifyJpegAsync(
            original.Path,
            original.SizeBytes,
            original.Sha256,
            "original.jpg",
            cancellationToken);
    }

    public static Task<VerifiedHardwareJpeg> VerifyPreviewAsync(
        HardwarePreviewJpegRecord preview,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(preview);
        return VerifyJpegAsync(
            preview.Path,
            preview.SizeBytes,
            preview.Sha256,
            "preview.jpg",
            cancellationToken);
    }

    internal static async Task<VerifiedHardwareJpeg> VerifyJpegAsync(
        string path,
        long expectedSize,
        string expectedSha256,
        string requiredFileName,
        CancellationToken cancellationToken)
    {
        ValidateExpectedRecord(path, expectedSize, expectedSha256, requiredFileName);
        EnsureRegularFile(path);
        await using var stream = OpenStableRead(path);
        var observed = await InspectJpegAsync(stream, cancellationToken).ConfigureAwait(false);
        if (observed.SizeBytes != expectedSize ||
            !string.Equals(observed.Sha256, expectedSha256, StringComparison.Ordinal))
        {
            throw new InvalidDataException("Camera Agent artifact size or SHA-256 changed after verification.");
        }

        return new VerifiedHardwareJpeg(path, observed.SizeBytes, observed.Sha256);
    }

    internal static FileStream OpenStableRead(string path) =>
        new(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            bufferSize: 128 * 1024,
            FileOptions.Asynchronous | FileOptions.SequentialScan);

    internal static async Task<(long SizeBytes, string Sha256)> InspectJpegAsync(
        Stream stream,
        CancellationToken cancellationToken,
        Stream? copyDestination = null)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[128 * 1024];
        long total = 0;
        byte first = 0;
        byte second = 0;
        byte previous = 0;
        byte last = 0;
        while (true)
        {
            var read = await stream.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
            if (read == 0)
            {
                break;
            }

            if (total == 0)
            {
                first = buffer[0];
                if (read > 1)
                {
                    second = buffer[1];
                }
            }
            else if (total == 1)
            {
                second = buffer[0];
            }

            for (var index = 0; index < read; index++)
            {
                previous = last;
                last = buffer[index];
            }

            hash.AppendData(buffer, 0, read);
            if (copyDestination is not null)
            {
                await copyDestination.WriteAsync(buffer.AsMemory(0, read), cancellationToken)
                    .ConfigureAwait(false);
            }

            total += read;
        }

        if (total < 4 || first != 0xFF || second != 0xD8 || previous != 0xFF || last != 0xD9)
        {
            throw new InvalidDataException("Artifact is not a complete JPEG file.");
        }

        return (total, Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant());
    }

    internal static void ValidateExpectedRecord(
        string path,
        long expectedSize,
        string expectedSha256,
        string requiredFileName)
    {
        if (string.IsNullOrWhiteSpace(path) || !Path.IsPathFullyQualified(path) || IsUncPath(path) ||
            !string.Equals(Path.GetFileName(path), requiredFileName, StringComparison.OrdinalIgnoreCase) ||
            expectedSize <= 0 || expectedSha256.Length != 64 ||
            !expectedSha256.All(character =>
                character is >= '0' and <= '9' or >= 'a' and <= 'f'))
        {
            throw new InvalidDataException("Camera Agent artifact metadata is invalid.");
        }
    }

    internal static void EnsureRegularFile(string path)
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
        var attributes = File.GetAttributes(path);
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new InvalidDataException("Camera Agent artifact must be a regular file.");
        }
    }

    internal static bool IsUncPath(string path) =>
        path.StartsWith("\\\\", StringComparison.Ordinal) ||
        path.StartsWith("//", StringComparison.Ordinal);
}

public sealed class HardwareOriginalExporter
{
    private readonly string _exportDirectory;
    private readonly Func<string, CancellationToken, Task>? _afterLockedVerificationForTesting;

    public HardwareOriginalExporter(string exportDirectory)
        : this(exportDirectory, afterLockedVerificationForTesting: null)
    {
    }

    internal HardwareOriginalExporter(
        string exportDirectory,
        Func<string, CancellationToken, Task>? afterLockedVerificationForTesting)
    {
        if (string.IsNullOrWhiteSpace(exportDirectory))
        {
            throw new ArgumentException("An export directory is required.", nameof(exportDirectory));
        }

        _exportDirectory = Path.GetFullPath(exportDirectory);
        _afterLockedVerificationForTesting = afterLockedVerificationForTesting;
    }

    public string ExportDirectory => _exportDirectory;

    public async Task<string> ExportAsync(
        HardwareRetainedOriginalRecord original,
        string transactionId,
        DateTimeOffset now,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(original);
        HardwareArtifactVerifier.ValidateExpectedRecord(
            original.Path,
            original.SizeBytes,
            original.Sha256,
            "original.jpg");
        if (transactionId.Length != 32 || !transactionId.All(character =>
                character is >= '0' and <= '9' or >= 'a' and <= 'f'))
        {
            throw new InvalidDataException("Export transaction ID is invalid.");
        }

        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_exportDirectory);
        Directory.CreateDirectory(_exportDirectory);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_exportDirectory);
        if ((File.GetAttributes(_exportDirectory) & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException("Export directory cannot be a reparse point.");
        }

        HardwareArtifactVerifier.EnsureRegularFile(original.Path);
        var safeAlias = original.CameraAlias switch
        {
            "CAM-A" => "CAM-A",
            "CAM-B" => "CAM-B",
            _ => throw new InvalidDataException("Export camera alias is invalid."),
        };
        var baseName = $"A0-single-{now.UtcDateTime:yyyyMMdd-HHmmssfff}-{safeAlias}-{transactionId[..8]}";
        var finalPath = UniqueDestinationPath(baseName);
        var partialPath = finalPath + ".partial";

        await using (var source = HardwareArtifactVerifier.OpenStableRead(original.Path))
        await using (var destination = new FileStream(
                         partialPath,
                         FileMode.CreateNew,
                         FileAccess.Write,
                         FileShare.None,
                         bufferSize: 128 * 1024,
                         FileOptions.Asynchronous | FileOptions.WriteThrough))
        {
            var observed = await HardwareArtifactVerifier
                .InspectJpegAsync(source, cancellationToken, destination)
                .ConfigureAwait(false);
            await destination.FlushAsync(cancellationToken).ConfigureAwait(false);
            destination.Flush(flushToDisk: true);
            if (observed.SizeBytes != original.SizeBytes ||
                !string.Equals(observed.Sha256, original.Sha256, StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    $"Original verification failed; diagnostic partial was retained: {partialPath}");
            }
        }

        await using var verifiedStagingFile =
            WindowsDurableFilePublisher.OpenLockedForVerifiedPublish(partialPath);
        var stagedRecord = await HardwareArtifactVerifier
            .InspectJpegAsync(verifiedStagingFile, cancellationToken)
            .ConfigureAwait(false);
        if (stagedRecord.SizeBytes != original.SizeBytes || stagedRecord.Sha256 != original.Sha256)
        {
            throw new InvalidDataException("Export reread verification failed.");
        }

        if (_afterLockedVerificationForTesting is not null)
        {
            await _afterLockedVerificationForTesting(partialPath, cancellationToken)
                .ConfigureAwait(false);
        }

        // The same write-locked, verified file handle is renamed. A competing
        // path replacement can therefore never become the accepted product JPEG.
        WindowsDurableFilePublisher.PublishLocked(
            verifiedStagingFile,
            finalPath,
            replaceExisting: false);
        return finalPath;
    }

    private string UniqueDestinationPath(string baseName)
    {
        for (var suffix = 0; suffix < 1000; suffix++)
        {
            var name = suffix == 0 ? $"{baseName}.jpg" : $"{baseName}-{suffix:D3}.jpg";
            var candidate = Path.GetFullPath(Path.Combine(_exportDirectory, name));
            if (!string.Equals(Path.GetDirectoryName(candidate), _exportDirectory, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException("Export path escaped the configured directory.");
            }

            if (!File.Exists(candidate) && !File.Exists(candidate + ".partial"))
            {
                return candidate;
            }
        }

        throw new IOException("A unique export filename could not be allocated.");
    }
}
