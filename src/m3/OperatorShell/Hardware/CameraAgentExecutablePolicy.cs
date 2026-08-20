using System.IO;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal static class CameraAgentExecutablePolicy
{
    private const string InvalidExecutableMessage =
        "Camera Agentの実行ファイルはアプリケーション配置先の直下にある固定ローカルドライブ上のEXEである必要があります。";

    internal static string Resolve(
        string baseDirectory,
        string candidate,
        Func<string, DriveType>? driveTypeResolver = null,
        Func<string, FileAttributes>? attributesResolver = null)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(baseDirectory) || string.IsNullOrWhiteSpace(candidate) ||
                !Path.IsPathFullyQualified(baseDirectory) || !Path.IsPathFullyQualified(candidate) ||
                IsNetworkOrDevicePath(baseDirectory) || IsNetworkOrDevicePath(candidate) ||
                HasAlternateDataStream(candidate) || HasTraversalSegment(candidate))
            {
                throw Invalid();
            }

            var normalizedBase = Path.TrimEndingDirectorySeparator(Path.GetFullPath(baseDirectory));
            var normalizedCandidate = Path.GetFullPath(candidate);
            if (!string.Equals(
                    Path.GetDirectoryName(normalizedCandidate),
                    normalizedBase,
                    StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(Path.GetExtension(normalizedCandidate), ".exe", StringComparison.OrdinalIgnoreCase))
            {
                throw Invalid();
            }

            var root = Path.GetPathRoot(normalizedCandidate);
            if (string.IsNullOrEmpty(root) ||
                (driveTypeResolver ?? (path => new DriveInfo(path).DriveType))(root) != DriveType.Fixed)
            {
                throw Invalid();
            }

            var readAttributes = attributesResolver ?? File.GetAttributes;
            if (!Directory.Exists(normalizedBase) || !File.Exists(normalizedCandidate))
            {
                throw Invalid();
            }

            EnsureReparseFreeDirectoryChain(normalizedBase, readAttributes);
            var fileAttributes = readAttributes(normalizedCandidate);
            if ((fileAttributes & (FileAttributes.Directory | FileAttributes.ReparsePoint | FileAttributes.Device)) != 0)
            {
                throw Invalid();
            }

            return normalizedCandidate;
        }
        catch (ArgumentException exception) when (exception.Message == InvalidExecutableMessage)
        {
            throw;
        }
        catch (Exception exception) when (exception is ArgumentException or IOException or UnauthorizedAccessException or NotSupportedException or System.Security.SecurityException)
        {
            throw Invalid();
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

    private static bool HasTraversalSegment(string value) =>
        value.Split(['\\', '/'], StringSplitOptions.RemoveEmptyEntries)
            .Any(segment => segment is "." or "..");

    private static void EnsureReparseFreeDirectoryChain(
        string directory,
        Func<string, FileAttributes> readAttributes)
    {
        var root = Path.GetPathRoot(directory) ?? throw Invalid();
        var current = root;
        foreach (var segment in directory[root.Length..]
                     .Split(['\\', '/'], StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, segment);
            if ((readAttributes(current) & FileAttributes.ReparsePoint) != 0)
            {
                throw Invalid();
            }
        }
    }

    private static ArgumentException Invalid() => new(InvalidExecutableMessage);
}
