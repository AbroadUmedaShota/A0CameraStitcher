using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace A0CameraStitcher.M3.Foundation.DualCamera;

public sealed class M2OfflineStitcherProcessAdapter : ITestSyntheticCamera, IOfflineStitcherAdapter
{
    private const int MaximumRetainedDiagnosticCharacters = 64 * 1024;
    private static readonly TimeSpan TerminationAndDrainTimeout = TimeSpan.FromSeconds(5);
    private readonly string _executablePath;

    public M2OfflineStitcherProcessAdapter(string executablePath)
    {
        if (string.IsNullOrWhiteSpace(executablePath))
        {
            throw new ArgumentException("The M2 adapter executable path is required.", nameof(executablePath));
        }
        _executablePath = Path.GetFullPath(executablePath);
        if (!File.Exists(_executablePath))
        {
            throw new FileNotFoundException("The M2 adapter executable was not found.", _executablePath);
        }
    }

    public async Task<string> CaptureAsync(
        string alias,
        Guid transactionId,
        string destinationPath,
        CancellationToken cancellationToken)
    {
        _ = transactionId;
        if (alias is not ("CAM-A" or "CAM-B"))
        {
            throw new ArgumentOutOfRangeException(nameof(alias));
        }
        Directory.CreateDirectory(Path.GetDirectoryName(destinationPath)!);
        await RunAsync(
            ["generate-test-synthetic", "--alias", alias, "--output", destinationPath, "--width", "16", "--height", "8"],
            cancellationToken).ConfigureAwait(false);
        return destinationPath;
    }

    public async Task<OfflineStitchArtifact> StitchAsync(
        IReadOnlyList<CanonicalJpegOriginal> originals,
        string outputJobDirectory,
        DualCameraRigProfile profile,
        Guid stitchJobId,
        Guid captureTransactionId,
        DateTimeOffset completedAtUtc,
        CancellationToken cancellationToken)
    {
        if (completedAtUtc.Offset != TimeSpan.Zero)
        {
            throw new ArgumentException("The StitchJob completion time must be UTC.", nameof(completedAtUtc));
        }

        var cameraA = originals.Single(original => original.Alias == "CAM-A");
        var cameraB = originals.Single(original => original.Alias == "CAM-B");
        var arguments = new List<string>
        {
            "stitch",
            "--camera-a", cameraA.Path,
            "--camera-b", cameraB.Path,
            "--job-directory", outputJobDirectory,
            "--profile-id", profile.ProfileId,
            "--status", profile.Status == DualCameraProfileStatus.Approved ? "approved" : "draft",
            "--schema-version", profile.SchemaVersion,
            "--provenance", profile.Provenance,
            "--measured-at", profile.MeasuredAtUtc.ToUnixTimeSeconds().ToString(CultureInfo.InvariantCulture),
            "--valid-until", profile.ValidUntilUtc.ToUnixTimeSeconds().ToString(CultureInfo.InvariantCulture),
            "--assessed-at", profile.AssessedAtUtc.ToUnixTimeSeconds().ToString(CultureInfo.InvariantCulture),
            "--width", profile.ExpectedInputWidth.ToString(CultureInfo.InvariantCulture),
            "--height", profile.ExpectedInputHeight.ToString(CultureInfo.InvariantCulture),
            "--matrix", string.Join(',', profile.CameraBToCameraA.Select(value => value.ToString("R", CultureInfo.InvariantCulture))),
            "--layout", profile.Layout,
            "--crop", string.Join(',', profile.Crop.Select(value => value.ToString(CultureInfo.InvariantCulture))),
            "--stitch-job-id", stitchJobId.ToString("N"),
            "--capture-transaction-id", captureTransactionId.ToString("N"),
            "--completed-at", completedAtUtc.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture),
        };
        var output = await RunAsync(arguments, cancellationToken).ConfigureAwait(false);
        var values = output.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries)
            .Select(line => line.Split('=', 2))
            .Where(parts => parts.Length == 2)
            .ToDictionary(parts => parts[0], parts => parts[1], StringComparer.Ordinal);
        if (!values.TryGetValue("result", out var result) || result != "stitched" ||
            !int.TryParse(values.GetValueOrDefault("width"), CultureInfo.InvariantCulture, out var width) ||
            !int.TryParse(values.GetValueOrDefault("height"), CultureInfo.InvariantCulture, out var height))
        {
            throw new InvalidDataException("The M2 adapter returned an invalid stitch response.");
        }

        // The job the adapter says it recorded has to be the job that was asked
        // for. A response naming a different StitchJob would leave this process
        // pointing at someone else's manifest.
        //
        // manifestFileName comes from the child process's stdout and must be a
        // bare file name. Path.Combine discards its first argument whenever the
        // second is rooted, so an adapter that reports an absolute path (or a
        // path containing separators) could redirect the existence check to an
        // arbitrary file on disk.
        var manifestFileName = values.GetValueOrDefault("manifest") ?? string.Empty;
        if (!string.Equals(values.GetValueOrDefault("stitchJobId"), stitchJobId.ToString("N"), StringComparison.Ordinal) ||
            !ManifestFileNameGuard.IsSafeManifestFileName(manifestFileName) ||
            !File.Exists(Path.Combine(Path.GetFullPath(outputJobDirectory), manifestFileName)))
        {
            throw new InvalidDataException(
                "The M2 adapter did not publish a StitchJob manifest for the requested job.");
        }

        return new OfflineStitchArtifact(
            Path.Combine(Path.GetFullPath(outputJobDirectory), "stitched.jpg"),
            width,
            height,
            values.GetValueOrDefault("profileId") ?? string.Empty,
            manifestFileName);
    }

    public async Task ValidateCanonicalJpegAsync(
        string jpegPath,
        int expectedWidth,
        int expectedHeight,
        CancellationToken cancellationToken)
    {
        var output = await RunAsync(
            [
                "validate-canonical-jpeg",
                "--input", jpegPath,
                "--width", expectedWidth.ToString(CultureInfo.InvariantCulture),
                "--height", expectedHeight.ToString(CultureInfo.InvariantCulture),
            ],
            cancellationToken).ConfigureAwait(false);
        if (!output.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries)
            .Contains("result=validated-canonical-jpeg", StringComparer.Ordinal))
        {
            throw new InvalidDataException("The M2 adapter returned an invalid canonical JPEG validation response.");
        }
    }

    public async Task ExportAsync(
        string stitchedJpeg,
        string destinationJpeg,
        CancellationToken cancellationToken)
    {
        await RunAsync(
            ["export", "--source", stitchedJpeg, "--destination", destinationJpeg],
            cancellationToken).ConfigureAwait(false);
    }

    private async Task<string> RunAsync(
        IReadOnlyList<string> arguments,
        CancellationToken cancellationToken)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = _executablePath,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }
        using var process = new Process { StartInfo = startInfo };
        if (!process.Start())
        {
            throw new InvalidOperationException("The M2 adapter process did not start.");
        }
        var standardOutput = ReadBoundedAsync(process.StandardOutput);
        var standardError = ReadBoundedAsync(process.StandardError);
        try
        {
            await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            await TerminateAndDrainAsync(process, standardOutput, standardError).ConfigureAwait(false);
            throw;
        }
        var (output, error) = await DrainOutputAsync(standardOutput, standardError).ConfigureAwait(false);
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException($"M2 adapter failed with exit code {process.ExitCode}: {error.Trim()}");
        }
        return output;
    }

    private static async Task<(string Output, string Error)> DrainOutputAsync(
        Task<string> standardOutput,
        Task<string> standardError)
    {
        using var timeout = new CancellationTokenSource(TerminationAndDrainTimeout);
        try
        {
            await Task.WhenAll(standardOutput, standardError).WaitAsync(timeout.Token).ConfigureAwait(false);
            return (await standardOutput.ConfigureAwait(false), await standardError.ConfigureAwait(false));
        }
        catch (OperationCanceledException) when (timeout.IsCancellationRequested)
        {
            ObserveFault(standardOutput);
            ObserveFault(standardError);
            throw new TimeoutException("The M2 adapter output drain exceeded the bounded termination window.");
        }
    }

    private static async Task TerminateAndDrainAsync(
        Process process,
        Task<string> standardOutput,
        Task<string> standardError)
    {
        using var timeout = new CancellationTokenSource(TerminationAndDrainTimeout);
        try
        {
            try
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true);
            }
            catch (Exception)
            {
                // Cleanup is best-effort and must never replace the caller's cancellation.
                // A natural-exit race and platform kill failures are both handled by the
                // independently bounded wait below.
            }

            try
            {
                await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
            }
            catch (Exception)
            {
                // Preserve the original OperationCanceledException from RunAsync.
            }

            try
            {
                await Task.WhenAll(standardOutput, standardError).WaitAsync(timeout.Token).ConfigureAwait(false);
            }
            catch (Exception)
            {
                // A closed or faulting redirected stream is cleanup evidence only.
            }
        }
        finally
        {
            ObserveFault(standardOutput);
            ObserveFault(standardError);
        }
    }

    private static async Task<string> ReadBoundedAsync(StreamReader reader)
    {
        var retained = new StringBuilder();
        var buffer = new char[4096];
        var truncated = false;
        while (true)
        {
            var read = await reader.ReadAsync(buffer.AsMemory()).ConfigureAwait(false);
            if (read == 0) break;
            var remaining = MaximumRetainedDiagnosticCharacters - retained.Length;
            if (remaining <= 0)
            {
                truncated = true;
                continue;
            }
            retained.Append(buffer, 0, Math.Min(remaining, read));
            truncated |= read > remaining;
        }
        if (truncated) retained.Append("\n[output truncated]");
        return retained.ToString();
    }

    private static void ObserveFault(Task task)
    {
        _ = task.ContinueWith(
            completed => _ = completed.Exception,
            CancellationToken.None,
            TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }
}

// Shared between this adapter and DualCameraProductFlow: both validate a
// manifest file name reported by (ultimately) child-process stdout before
// using it in Path.Combine/File.Exists.
internal static class ManifestFileNameGuard
{
    // Windows reserved device names: these resolve to a device rather than a
    // regular file even with an extension attached (e.g. "NUL.json" still
    // opens the NUL device), so File.Exists/File.Open on them does not behave
    // like a normal file-existence check.
    private static readonly HashSet<string> ReservedWindowsDeviceNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };

    internal static bool IsSafeManifestFileName(string candidate)
    {
        if (string.IsNullOrEmpty(candidate))
        {
            return false;
        }
        // Path.GetFileName(candidate) == candidate rejects any directory
        // separator or rooted path, but "." and ".." both round-trip through
        // GetFileName unchanged, so they need an explicit check.
        if (!string.Equals(Path.GetFileName(candidate), candidate, StringComparison.Ordinal) ||
            candidate is "." or "..")
        {
            return false;
        }
        // Windows resolves reserved names from the segment before the FIRST
        // period, with trailing spaces/periods stripped ("NUL.json.txt" and
        // "NUL " both reach the NUL device). GetFileNameWithoutExtension only
        // strips the last extension, so derive that first segment explicitly.
        var firstSegment = candidate.Split('.')[0].TrimEnd(' ', '.');
        return !ReservedWindowsDeviceNames.Contains(firstSegment);
    }
}
