using System.Text.Json;
using System.Security.Cryptography;

namespace A0CameraStitcher.M3.Foundation.DualCamera;

/// <summary>
/// Reads only the optional v2 review-navigation record beside a completed
/// stitched output. Any unavailable, old, malformed, or mismatched record is
/// deliberately indistinguishable from no navigation point to the UI.
/// </summary>
internal static class StitchSeamNavigationManifestReader
{
    private const string ManifestFileName = "stitch-job.manifest.json";
    private const string SchemaVersion = "a0.stitch-job-manifest.v2";
    private const string CoordinateSystem = "StitchedOutputPixelCenter.v1";
    private const long MaximumManifestBytes = 256 * 1024;

    internal static StitchSeamNavigationPoint? TryReadForStitchedOutput(string stitchedOutputPath)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(stitchedOutputPath) ||
                !string.Equals(Path.GetFileName(stitchedOutputPath), "stitched.jpg", StringComparison.OrdinalIgnoreCase))
            {
                return null;
            }

            var outputPath = Path.GetFullPath(stitchedOutputPath);
            var directory = Path.GetDirectoryName(outputPath);
            if (string.IsNullOrEmpty(directory)) return null;
            var manifestPath = Path.Combine(directory, ManifestFileName);
            var expectedOutputPath = Path.GetFullPath(Path.Combine(directory, "stitched.jpg"));
            if (!string.Equals(outputPath, expectedOutputPath, StringComparison.OrdinalIgnoreCase)) return null;

            var outputInfo = new FileInfo(outputPath);
            if (!outputInfo.Exists || outputInfo.Length <= 0) return null;

            var info = new FileInfo(manifestPath);
            if (!info.Exists || info.Length is <= 0 or > MaximumManifestBytes) return null;
            using var stream = new FileStream(manifestPath, FileMode.Open, FileAccess.Read, FileShare.Read);
            if (stream.Length is <= 0 or > MaximumManifestBytes) return null;
            using var document = JsonDocument.Parse(stream);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !HasExactProperties(root,
                    "schemaVersion", "stitchJobId", "captureTransactionId", "inputs", "rigProfile",
                    "engine", "output", "seamNavigation", "terminalResultState", "completedAtUtc",
                    "automaticRetryCount") ||
                !HasString(root, "schemaVersion", SchemaVersion) ||
                !HasString(root, "terminalResultState", "Succeeded"))
            {
                return null;
            }

            var output = root.GetProperty("output");
            if (output.ValueKind != JsonValueKind.Object ||
                !HasExactProperties(output, "relativePath", "sha256", "widthPixels", "heightPixels", "encodedSizeBytes") ||
                !HasString(output, "relativePath", "stitched.jpg") ||
                !output.TryGetProperty("sha256", out var sha256) || sha256.ValueKind != JsonValueKind.String || !IsLowercaseSha256(sha256.GetString()) ||
                !output.TryGetProperty("widthPixels", out var width) || width.ValueKind != JsonValueKind.Number || !width.TryGetInt32(out var outputWidth) || outputWidth <= 0 ||
                !output.TryGetProperty("heightPixels", out var height) || height.ValueKind != JsonValueKind.Number || !height.TryGetInt32(out var outputHeight) || outputHeight <= 0 ||
                !output.TryGetProperty("encodedSizeBytes", out var encodedSize) || encodedSize.ValueKind != JsonValueKind.Number || !encodedSize.TryGetInt64(out var expectedSize) || expectedSize != outputInfo.Length ||
                !string.Equals(ComputeSha256(outputPath), sha256.GetString(), StringComparison.Ordinal))
            {
                return null;
            }

            var seam = root.GetProperty("seamNavigation");
            if (seam.ValueKind != JsonValueKind.Object ||
                !HasString(seam, "coordinateSystem", CoordinateSystem) ||
                !seam.TryGetProperty("available", out var available) || available.ValueKind != JsonValueKind.True)
            {
                return null;
            }
            if (!HasExactProperties(seam, "coordinateSystem", "available", "xPixels", "yPixels") ||
                !seam.TryGetProperty("xPixels", out var x) || x.ValueKind != JsonValueKind.Number || !x.TryGetInt32(out var xPixels) ||
                !seam.TryGetProperty("yPixels", out var y) || y.ValueKind != JsonValueKind.Number || !y.TryGetInt32(out var yPixels) ||
                xPixels < 0 || yPixels < 0 || xPixels >= outputWidth || yPixels >= outputHeight)
            {
                return null;
            }
            return new StitchSeamNavigationPoint(xPixels, yPixels, outputWidth, outputHeight);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException or ArgumentException or NotSupportedException or InvalidOperationException)
        {
            return null;
        }
    }

    private static bool HasString(JsonElement objectElement, string name, string expected) =>
        objectElement.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String &&
        string.Equals(value.GetString(), expected, StringComparison.Ordinal);

    private static bool IsLowercaseSha256(string? value) =>
        value is { Length: 64 } && value.All(character =>
            character is >= '0' and <= '9' || character is >= 'a' and <= 'f');

    private static string ComputeSha256(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static bool HasExactProperties(JsonElement objectElement, params string[] names)
    {
        var properties = objectElement.EnumerateObject().Select(property => property.Name).ToArray();
        var actual = properties.ToHashSet(StringComparer.Ordinal);
        return properties.Length == names.Length && actual.Count == names.Length && actual.SetEquals(names);
    }
}

internal sealed record StitchSeamNavigationPoint(int XPixels, int YPixels, int OutputWidth, int OutputHeight);
