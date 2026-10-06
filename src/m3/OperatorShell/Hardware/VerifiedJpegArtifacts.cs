using System.IO;
using System.Security.Cryptography;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

public sealed record VerifiedHardwareJpeg(
    string Path,
    long SizeBytes,
    string Sha256);

public static class HardwareArtifactVerifier
{
    private const int SingleOriginalWidth = 7360;
    private const int SingleOriginalHeight = 4912;

    public static Task<VerifiedHardwareJpeg> VerifyOriginalAsync(
        HardwareRetainedOriginalRecord original,
        string expectedPath,
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
            expectedPath,
            cancellationToken);
    }

    public static Task<VerifiedHardwareJpeg> VerifyPreviewAsync(
        HardwarePreviewJpegRecord preview,
        string expectedPath,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(preview);
        return VerifyJpegAsync(
            preview.Path,
            preview.SizeBytes,
            preview.Sha256,
            "preview.jpg",
            expectedPath,
            cancellationToken);
    }

    internal static async Task<VerifiedHardwareJpeg> VerifyJpegAsync(
        string path,
        long expectedSize,
        string expectedSha256,
        string requiredFileName,
        string expectedPath,
        CancellationToken cancellationToken)
    {
        ValidateExpectedRecord(path, expectedSize, expectedSha256, requiredFileName, expectedPath);
        EnsureRegularFile(path);
        await using var stream = OpenStableRead(path);
        var observed = await InspectJpegAsync(
            stream,
            cancellationToken,
            requiredFileName.Equals("original.jpg", StringComparison.OrdinalIgnoreCase)
                ? (SingleOriginalWidth, SingleOriginalHeight)
                : null).ConfigureAwait(false);
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
        (int Width, int Height)? expectedDimensions = null,
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

        if (expectedDimensions is { } dimensions)
        {
            await VerifyJpegDimensionsAsync(
                stream,
                dimensions.Width,
                dimensions.Height,
                cancellationToken).ConfigureAwait(false);
        }

        return (total, Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant());
    }

    private static async Task VerifyJpegDimensionsAsync(
        Stream stream,
        int expectedWidth,
        int expectedHeight,
        CancellationToken cancellationToken)
    {
        if (!stream.CanSeek)
        {
            throw new InvalidDataException("Original JPEG dimensions cannot be verified on a non-seekable stream.");
        }

        stream.Position = 2;
        var markerBytes = new byte[2];
        while (stream.Position + 1 < stream.Length)
        {
            await stream.ReadExactlyAsync(markerBytes.AsMemory(0, 1), cancellationToken).ConfigureAwait(false);
            if (markerBytes[0] != 0xFF)
            {
                throw new InvalidDataException("Original JPEG marker structure is invalid.");
            }

            do
            {
                await stream.ReadExactlyAsync(markerBytes.AsMemory(0, 1), cancellationToken).ConfigureAwait(false);
            }
            while (markerBytes[0] == 0xFF);

            var marker = markerBytes[0];
            if (marker is 0x00 or 0xD9 or 0xDA)
            {
                break;
            }
            if (marker == 0x01 || marker is >= 0xD0 and <= 0xD7)
            {
                continue;
            }

            await stream.ReadExactlyAsync(markerBytes, cancellationToken).ConfigureAwait(false);
            var segmentLength = (markerBytes[0] << 8) | markerBytes[1];
            if (segmentLength < 2 || segmentLength - 2 > stream.Length - stream.Position)
            {
                throw new InvalidDataException("Original JPEG segment length is invalid.");
            }

            var isStartOfFrame = marker is >= 0xC0 and <= 0xCF and not (0xC4 or 0xC8 or 0xCC);
            if (isStartOfFrame)
            {
                if (segmentLength < 7)
                {
                    break;
                }
                var frameHeader = new byte[5];
                await stream.ReadExactlyAsync(frameHeader, cancellationToken).ConfigureAwait(false);
                var height = (frameHeader[1] << 8) | frameHeader[2];
                var width = (frameHeader[3] << 8) | frameHeader[4];
                if (width != expectedWidth || height != expectedHeight)
                {
                    throw new InvalidDataException(
                        $"Original JPEG dimensions must be {expectedWidth}x{expectedHeight}; observed {width}x{height}.");
                }
                return;
            }

            stream.Seek(segmentLength - 2, SeekOrigin.Current);
        }

        throw new InvalidDataException("Original JPEG dimensions could not be verified.");
    }

    internal static void ValidateExpectedRecord(
        string path,
        long expectedSize,
        string expectedSha256,
        string requiredFileName,
        string expectedPath)
    {
        if (string.IsNullOrWhiteSpace(path) || !Path.IsPathFullyQualified(path) || IsUncPath(path) ||
            !string.Equals(Path.GetFileName(path), requiredFileName, StringComparison.OrdinalIgnoreCase) ||
            expectedSize <= 0 || expectedSha256.Length != 64 ||
            !expectedSha256.All(character =>
                character is >= '0' and <= '9' or >= 'a' and <= 'f'))
        {
            throw new InvalidDataException("Camera Agent artifact metadata is invalid.");
        }

        // expectedPath is the canonical location the caller independently
        // derived (HardwareAgentArtifactLayout.OriginalPath/PreviewPath) from
        // values it already owns or has separately validated: the
        // --artifacts-root this process itself passed to the agent, plus the
        // agent's run/transaction/alias, which HardwareCameraAgentProtocol
        // validates the shape of (and, for transactionId, that it matches the
        // transaction requested) before this code ever runs. Requiring exact
        // equality here -- not merely containment under the artifacts root --
        // is a provenance guarantee and accident detector: a Camera Agent
        // that names a file outside its own run/transaction directory (its
        // own leftovers, a different transaction's original, or an
        // altogether unrelated local file with a self-supplied matching
        // size/hash) is rejected even though authentication of "is this our
        // Camera Agent" is already handled upstream by the named pipe's ACL
        // and CurrentUserOnly restriction.
        if (string.IsNullOrWhiteSpace(expectedPath) || !Path.IsPathFullyQualified(expectedPath))
        {
            throw new InvalidDataException("The Camera Agent canonical artifact path is invalid.");
        }
        if (!string.Equals(
                Path.GetFullPath(path),
                Path.GetFullPath(expectedPath),
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "Camera Agent artifact path is not the canonical location for this run.");
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
        string expectedPath,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(original);
        HardwareArtifactVerifier.ValidateExpectedRecord(
            original.Path,
            original.SizeBytes,
            original.Sha256,
            "original.jpg",
            expectedPath);
        ValidateTransactionId(transactionId);
        EnsureExportDirectoryIsSafe();

        var safeAlias = original.CameraAlias switch
        {
            "CAM-A" => "CAM-A",
            "CAM-B" => "CAM-B",
            _ => throw new InvalidDataException("Export camera alias is invalid."),
        };
        var baseName = $"A0-single-{now.UtcDateTime:yyyyMMdd-HHmmssfff}-{safeAlias}-{transactionId[..8]}";
        var (lockedFile, finalPath) = await StageVerifiedOriginalAsync(
                original.Path, original.SizeBytes, original.Sha256, baseName, cancellationToken)
            .ConfigureAwait(false);

        if (_afterLockedVerificationForTesting is not null)
        {
            await _afterLockedVerificationForTesting(finalPath + ".partial", cancellationToken)
                .ConfigureAwait(false);
        }

        // The same write-locked, verified file handle is renamed. A competing
        // path replacement can therefore never become the accepted product JPEG.
        await using (lockedFile)
        {
            WindowsDurableFilePublisher.PublishLocked(
                lockedFile,
                finalPath,
                replaceExisting: false);
        }
        return finalPath;
    }

    /// <summary>
    /// Exports one or two already-verified CaptureRecoveryOnly dual-camera originals
    /// (Succeeded: CAM-A and CAM-B; FailedPartial with a CAM-A-only retained original: one
    /// file) as byte-identical copies. Every original is written, locked, and reread-verified
    /// before any rename occurs — one failed original can therefore never leave a lone sibling
    /// published under a final name (see GitHub Issue #226).
    /// </summary>
    public async Task<IReadOnlyList<string>> ExportDualOriginalsAsync(
        IReadOnlyList<CanonicalJpegOriginal> originals,
        string transactionId,
        string transactionDirectory,
        DateTimeOffset now,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(originals);
        if (originals.Count is not (1 or 2))
        {
            throw new InvalidDataException("Export requires one or two retained dual-camera originals.");
        }
        if (!originals.Select(original => original.Alias)
                .SequenceEqual(OrderedDualAliases.Take(originals.Count), StringComparer.Ordinal))
        {
            throw new InvalidDataException("Export originals must be exactly CAM-A, then CAM-A/CAM-B in order.");
        }
        ValidateTransactionId(transactionId);
        if (string.IsNullOrWhiteSpace(transactionDirectory) || !Path.IsPathFullyQualified(transactionDirectory))
        {
            throw new InvalidDataException("Export transaction directory is invalid.");
        }
        EnsureExportDirectoryIsSafe();

        var staged = new List<(FileStream LockedFile, string FinalPath)>(originals.Count);
        try
        {
            foreach (var original in originals)
            {
                var expectedPath = Path.GetFullPath(
                    Path.Combine(transactionDirectory, original.Alias, "original.jpg"));
                HardwareArtifactVerifier.ValidateExpectedRecord(
                    original.Path, original.SizeBytes, original.Sha256, "original.jpg", expectedPath);

                var baseName =
                    $"A0-dual-{now.UtcDateTime:yyyyMMdd-HHmmssfff}-{original.Alias}-{transactionId[..8]}";
                staged.Add(await StageVerifiedOriginalAsync(
                        original.Path, original.SizeBytes, original.Sha256, baseName, cancellationToken)
                    .ConfigureAwait(false));
            }

            // Only now that every original has been written, locked, and reread-verified do
            // any of the staged files get their final name.
            var finalPaths = new List<string>(staged.Count);
            foreach (var (lockedFile, finalPath) in staged)
            {
                WindowsDurableFilePublisher.PublishLocked(lockedFile, finalPath, replaceExisting: false);
                finalPaths.Add(finalPath);
            }
            return finalPaths;
        }
        finally
        {
            foreach (var (lockedFile, _) in staged)
            {
                await lockedFile.DisposeAsync().ConfigureAwait(false);
            }
        }
    }

    private void EnsureExportDirectoryIsSafe()
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_exportDirectory);
        Directory.CreateDirectory(_exportDirectory);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_exportDirectory);
        if ((File.GetAttributes(_exportDirectory) & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException("Export directory cannot be a reparse point.");
        }
    }

    private static void ValidateTransactionId(string transactionId)
    {
        if (transactionId.Length != 32 || !transactionId.All(character =>
                character is >= '0' and <= '9' or >= 'a' and <= 'f'))
        {
            throw new InvalidDataException("Export transaction ID is invalid.");
        }
    }

    /// <summary>
    /// Writes <paramref name="sourcePath"/> to a uniquely-named ".partial" file under
    /// <see cref="_exportDirectory"/>, verifies the copy byte-for-byte against the expected
    /// size/SHA-256 while writing it, then opens and rereads the write-locked result once more
    /// before handing the still-open, still-locked handle back to the caller. The caller alone
    /// decides when (or whether) to publish it under its final name.
    /// </summary>
    private async Task<(FileStream LockedFile, string FinalPath)> StageVerifiedOriginalAsync(
        string sourcePath,
        long expectedSizeBytes,
        string expectedSha256,
        string baseName,
        CancellationToken cancellationToken)
    {
        HardwareArtifactVerifier.EnsureRegularFile(sourcePath);
        var finalPath = UniqueDestinationPath(baseName);
        var partialPath = finalPath + ".partial";

        await using (var source = HardwareArtifactVerifier.OpenStableRead(sourcePath))
        await using (var destination = new FileStream(
                         partialPath,
                         FileMode.CreateNew,
                         FileAccess.Write,
                         FileShare.None,
                         bufferSize: 128 * 1024,
                         FileOptions.Asynchronous | FileOptions.WriteThrough))
        {
            var observed = await HardwareArtifactVerifier
                .InspectJpegAsync(source, cancellationToken, (7360, 4912), destination)
                .ConfigureAwait(false);
            await destination.FlushAsync(cancellationToken).ConfigureAwait(false);
            destination.Flush(flushToDisk: true);
            if (observed.SizeBytes != expectedSizeBytes ||
                !string.Equals(observed.Sha256, expectedSha256, StringComparison.Ordinal))
            {
                throw new InvalidDataException(
                    $"Original verification failed; diagnostic partial was retained: {partialPath}");
            }
        }

        var lockedFile = WindowsDurableFilePublisher.OpenLockedForVerifiedPublish(partialPath);
        try
        {
            var stagedRecord = await HardwareArtifactVerifier
                .InspectJpegAsync(lockedFile, cancellationToken, (7360, 4912))
                .ConfigureAwait(false);
            if (stagedRecord.SizeBytes != expectedSizeBytes || stagedRecord.Sha256 != expectedSha256)
            {
                throw new InvalidDataException("Export reread verification failed.");
            }
        }
        catch
        {
            await lockedFile.DisposeAsync().ConfigureAwait(false);
            throw;
        }

        return (lockedFile, finalPath);
    }

    private static readonly IReadOnlyList<string> OrderedDualAliases = Array.AsReadOnly(["CAM-A", "CAM-B"]);

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
