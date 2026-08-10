using System.IO;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal sealed record HardwareSingleStoragePaths(
    string StateDirectory,
    string DefaultExportDirectory,
    string PreferencesPath,
    string CaptureProfilePath,
    string SingleIdentityV3Path)
{
    public string ExportDirectory => DefaultExportDirectory;

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
        var productRoot = Path.Combine(root, "A0CameraStitcher");
        return new HardwareSingleStoragePaths(
            Path.Combine(productRoot, "hardware-single"),
            Path.Combine(productRoot, "Exports"),
            Path.Combine(productRoot, "hardware-single", "preferences.json"),
            Path.Combine(productRoot, "camera-agent", "approved-single-capture-profile.json"),
            Path.Combine(productRoot, "phase0", "single-identity-v3.json"));
    }
}
