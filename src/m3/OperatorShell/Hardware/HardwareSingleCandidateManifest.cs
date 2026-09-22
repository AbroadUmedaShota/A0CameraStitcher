using System.IO;
using System.Text.Json;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal enum HardwareSingleCandidateManifestStatus
{
    Missing,
    SoftwareOnly,
    SdkInclusionDeclared,
    Invalid,
}

/// <summary>
/// Read-only packaging inspection for the launcher. This is deliberately not a
/// hardware-readiness check: it neither starts the agent nor loads the SDK.
/// </summary>
internal sealed record HardwareSingleCandidateInspection(
    bool ExecutableExists,
    HardwareSingleCandidateManifestStatus ManifestStatus)
{
    // This only permits entry to the existing fail-closed readiness screen. It
    // is not proof that an SDK can load or that a camera is connected.
    public bool CanStartHardware =>
        ExecutableExists && ManifestStatus is
            HardwareSingleCandidateManifestStatus.Missing or
            HardwareSingleCandidateManifestStatus.SdkInclusionDeclared;

    public string AvailabilityText =>
        !ExecutableExists
            ? "実行ファイル未検出（SDK・実機接続は未確認）"
            : ManifestStatus switch
            {
                HardwareSingleCandidateManifestStatus.SoftwareOnly =>
                    "SDK非同梱のソフトウェア確認用候補。カメラ未照会",
                HardwareSingleCandidateManifestStatus.Invalid =>
                    "候補manifestが不正です。実機開始不可",
                HardwareSingleCandidateManifestStatus.SdkInclusionDeclared =>
                    "実行ファイルあり（SDK・実機接続は未確認）",
                _ => "実行ファイルあり（SDK・実機接続は未確認）",
            };
}

internal static class HardwareSingleCandidateManifest
{
    private const string ManifestFileName = "candidate.manifest.json";
    private const int MaximumManifestBytes = 64 * 1024;

    public static HardwareSingleCandidateInspection Inspect(string cameraAgentExecutablePath)
    {
        var executableExists = false;
        try
        {
            if (string.IsNullOrWhiteSpace(cameraAgentExecutablePath) ||
                !Path.IsPathFullyQualified(cameraAgentExecutablePath) ||
                IsNetworkOrDevicePath(cameraAgentExecutablePath) ||
                HasAlternateDataStream(cameraAgentExecutablePath))
            {
                return Invalid();
            }

            var normalizedAgentPath = Path.GetFullPath(cameraAgentExecutablePath);
            EnsureFixedLocalRegularFile(normalizedAgentPath);
            executableExists = true;
            var applicationDirectory = Path.GetDirectoryName(normalizedAgentPath);
            if (string.IsNullOrWhiteSpace(applicationDirectory))
            {
                return Invalid();
            }

            EnsureReparseFreeDirectoryChain(applicationDirectory);
            var candidateDirectory = Directory.GetParent(applicationDirectory)?.FullName;
            if (string.IsNullOrWhiteSpace(candidateDirectory))
            {
                return Invalid();
            }

            var manifestPath = Path.Combine(candidateDirectory, ManifestFileName);
            FileAttributes manifestAttributes;
            try
            {
                manifestAttributes = File.GetAttributes(manifestPath);
            }
            catch (FileNotFoundException)
            {
                return Missing(executableExists: true);
            }
            catch (DirectoryNotFoundException)
            {
                return Missing(executableExists: true);
            }

            if ((manifestAttributes & (FileAttributes.Directory | FileAttributes.ReparsePoint | FileAttributes.Device)) != 0)
            {
                return Invalid();
            }

            using var stream = new FileStream(
                manifestPath,
                FileMode.Open,
                FileAccess.Read,
                FileShare.Read);
            if (stream.Length > MaximumManifestBytes)
            {
                return Invalid();
            }

            var bytes = ReadBounded(stream);
            using var document = JsonDocument.Parse(bytes, new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
            });

            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object || HasDuplicateProperties(root) ||
                !root.TryGetProperty("schemaVersion", out var schemaVersion) ||
                !root.TryGetProperty("status", out var status) ||
                !root.TryGetProperty("sdkIncluded", out var sdkIncluded) ||
                schemaVersion.ValueKind != JsonValueKind.Number || schemaVersion.GetInt32() != 1 ||
                status.ValueKind != JsonValueKind.String ||
                sdkIncluded.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
            {
                return Invalid();
            }

            if (status.GetString() != "local-software-candidate")
            {
                return Invalid();
            }

            return !sdkIncluded.GetBoolean()
                ? new HardwareSingleCandidateInspection(
                    ExecutableExists: true,
                    HardwareSingleCandidateManifestStatus.SoftwareOnly)
                : new HardwareSingleCandidateInspection(
                    ExecutableExists: true,
                    HardwareSingleCandidateManifestStatus.SdkInclusionDeclared);
        }
        catch (FileNotFoundException)
        {
            return Missing(executableExists);
        }
        catch (DirectoryNotFoundException)
        {
            return Missing(executableExists);
        }
        catch (InvalidDataException)
        {
            return Invalid();
        }
        catch (IOException)
        {
            return Invalid();
        }
        catch (UnauthorizedAccessException)
        {
            return Invalid();
        }
        catch (JsonException)
        {
            return Invalid();
        }
        catch (ArgumentException)
        {
            return Invalid();
        }
        catch (FormatException)
        {
            return Invalid();
        }
        catch (InvalidOperationException)
        {
            return Invalid();
        }
        catch (NotSupportedException)
        {
            return Invalid();
        }
        catch (System.Security.SecurityException)
        {
            return Invalid();
        }
    }

    private static HardwareSingleCandidateInspection Invalid() =>
        new(ExecutableExists: true, HardwareSingleCandidateManifestStatus.Invalid);

    private static HardwareSingleCandidateInspection Missing(bool executableExists) =>
        new(executableExists, HardwareSingleCandidateManifestStatus.Missing);

    private static byte[] ReadBounded(Stream stream)
    {
        var buffer = new byte[MaximumManifestBytes + 1];
        var total = 0;
        while (total < buffer.Length)
        {
            var read = stream.Read(buffer, total, buffer.Length - total);
            if (read == 0)
            {
                break;
            }

            total += read;
        }

        if (total > MaximumManifestBytes)
        {
            throw new InvalidDataException("Candidate manifest exceeds the bounded read limit.");
        }

        return buffer[..total];
    }

    private static void EnsureFixedLocalRegularFile(string path)
    {
        var root = Path.GetPathRoot(path);
        if (string.IsNullOrEmpty(root) || new DriveInfo(root).DriveType != DriveType.Fixed)
        {
            throw new InvalidOperationException();
        }

        var attributes = File.GetAttributes(path);
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint | FileAttributes.Device)) != 0)
        {
            throw new InvalidOperationException();
        }
    }

    private static void EnsureReparseFreeDirectoryChain(string directory)
    {
        var root = Path.GetPathRoot(directory) ?? throw new InvalidOperationException();
        var current = root;
        foreach (var segment in directory[root.Length..].Split(['\\', '/'], StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, segment);
            if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
            {
                throw new InvalidOperationException();
            }
        }
    }

    private static bool IsNetworkOrDevicePath(string value) =>
        value.StartsWith("\\\\", StringComparison.Ordinal) ||
        value.StartsWith("//", StringComparison.Ordinal) ||
        value.StartsWith("\\\\?\\", StringComparison.Ordinal) ||
        value.StartsWith("\\\\.\\", StringComparison.Ordinal);

    private static bool HasAlternateDataStream(string value)
    {
        var firstColon = value.IndexOf(':');
        return firstColon != 1 || value.IndexOf(':', firstColon + 1) >= 0;
    }

    private static bool HasDuplicateProperties(JsonElement element)
    {
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var property in element.EnumerateObject())
        {
            if (!names.Add(property.Name))
            {
                return true;
            }
        }

        return false;
    }
}
