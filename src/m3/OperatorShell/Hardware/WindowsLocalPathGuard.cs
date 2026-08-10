using System.IO;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal static class WindowsLocalPathGuard
{
    public static void EnsureExistingChainIsLocalAndNotReparse(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !Path.IsPathFullyQualified(path))
        {
            throw new InvalidDataException("A fully-qualified local Windows path is required.");
        }

        var fullPath = Path.GetFullPath(path);
        if (fullPath.StartsWith("\\\\", StringComparison.Ordinal) ||
            fullPath.StartsWith("//", StringComparison.Ordinal) ||
            fullPath.StartsWith("\\\\?\\", StringComparison.Ordinal) ||
            fullPath.AsSpan(2).Contains(':'))
        {
            throw new InvalidDataException("UNC, device, and alternate-stream paths are not allowed.");
        }

        var root = Path.GetPathRoot(fullPath);
        if (string.IsNullOrEmpty(root) || root.Length != 3 || root[1] != ':')
        {
            throw new InvalidDataException("A drive-qualified local Windows path is required.");
        }

        DriveType driveType;
        try
        {
            driveType = new DriveInfo(root).DriveType;
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or ArgumentException)
        {
            throw new InvalidDataException("The Windows drive type could not be verified.", exception);
        }

        if (!IsSupportedLocalDrive(driveType))
        {
            throw new InvalidDataException(
                "Application state, Camera Agent artifacts, and exports require a fixed local drive.");
        }

        var current = root;
        var segments = fullPath[root.Length..].Split(
            [Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar],
            StringSplitOptions.RemoveEmptyEntries);
        foreach (var segment in segments)
        {
            current = Path.Combine(current, segment);
            FileAttributes attributes;
            try
            {
                attributes = File.GetAttributes(current);
            }
            catch (Exception exception) when (
                exception is FileNotFoundException or DirectoryNotFoundException)
            {
                continue;
            }

            if ((attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new InvalidDataException("The local path chain cannot contain a reparse point.");
            }
        }
    }

    internal static bool IsSupportedLocalDrive(DriveType driveType) => driveType == DriveType.Fixed;
}
