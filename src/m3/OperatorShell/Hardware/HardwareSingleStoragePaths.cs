using System.IO;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal sealed record HardwareSingleStoragePaths(string StateDirectory, string ExportDirectory)
{
    public static HardwareSingleStoragePaths Resolve(string localApplicationData)
    {
        if (string.IsNullOrWhiteSpace(localApplicationData) ||
            !Path.IsPathFullyQualified(localApplicationData))
        {
            throw new InvalidDataException(
                "Windows LocalApplicationData must be an absolute fixed-local path.");
        }

        var root = Path.GetFullPath(localApplicationData);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(root);
        return new HardwareSingleStoragePaths(
            Path.Combine(root, "A0CameraStitcher", "hardware-single"),
            Path.Combine(root, "A0CameraStitcher", "Exports"));
    }
}
