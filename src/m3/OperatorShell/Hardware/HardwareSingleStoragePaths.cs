using System.IO;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal sealed record HardwareSingleStoragePaths(
    string StateDirectory,
    string DefaultExportDirectory,
    string PreferencesPath,
    string CaptureProfilePath,
    string SingleIdentityV3Path,
    string AgentArtifactsRoot,
    string HandoffEvidenceRoot)
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
            Path.Combine(productRoot, "phase0", "single-identity-v3.json"),
            // Must match ProductionHardwareCameraAgentConfig::Defaults() in
            // hardware_camera_agent.cpp: root = sdk_identity_map.parent_path()
            // / "camera-agent" (i.e. <LocalAppData>/A0CameraStitcher/phase0/
            // camera-agent), artifacts_root = root / "artifacts". The "phase0"
            // segment is easy to drop by analogy with CaptureProfilePath above
            // (which has no "phase0" segment) -- don't; it would point this
            // process's canonical-path checks at a different directory than
            // where the agent actually writes by default.
            Path.Combine(productRoot, "phase0", "camera-agent", "artifacts"),
            Path.Combine(productRoot, "hardware-single", "handoff-evidence"));
    }
}
