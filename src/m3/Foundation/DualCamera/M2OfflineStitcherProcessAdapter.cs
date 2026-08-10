using System.Diagnostics;
using System.Globalization;

namespace A0CameraStitcher.M3.Foundation.DualCamera;

public sealed class M2OfflineStitcherProcessAdapter : ITestSyntheticCamera, IOfflineStitcherAdapter
{
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
        CancellationToken cancellationToken)
    {
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
        return new OfflineStitchArtifact(
            Path.Combine(Path.GetFullPath(outputJobDirectory), "stitched.jpg"),
            width,
            height,
            values.GetValueOrDefault("profileId") ?? string.Empty);
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
        var standardOutput = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var standardError = process.StandardError.ReadToEndAsync(cancellationToken);
        try
        {
            await process.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                await process.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
            }
            throw;
        }
        var output = await standardOutput.ConfigureAwait(false);
        var error = await standardError.ConfigureAwait(false);
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException($"M2 adapter failed with exit code {process.ExitCode}: {error.Trim()}");
        }
        return output;
    }
}
