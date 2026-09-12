using System.Diagnostics;
using System.IO;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal sealed class HardwareSingleStoragePaths
{
    private HardwareSingleStoragePaths(string localApplicationData)
    {
        LocalApplicationData = localApplicationData;
        var productRoot = Path.Combine(localApplicationData, "A0CameraStitcher");
        var agentRoot = Path.Combine(productRoot, "phase0", "camera-agent");
        StateDirectory = Path.Combine(productRoot, "hardware-single");
        DefaultExportDirectory = Path.Combine(productRoot, "Exports");
        PreferencesPath = Path.Combine(StateDirectory, "preferences.json");
        CaptureProfilePath = Path.Combine(productRoot, "camera-agent", "approved-single-capture-profile.json");
        SingleIdentityV3Path = Path.Combine(productRoot, "phase0", "single-identity-v3.json");
        // Keep the phase0 segment: these three roots match native Defaults().
        // The separately approved capture-profile path intentionally has no phase0.
        AgentArtifactsRoot = Path.Combine(agentRoot, "artifacts");
        AgentReportsRoot = Path.Combine(agentRoot, "reports");
        AgentTransactionStateRoot = Path.Combine(agentRoot, "transactions");
        HandoffEvidenceRoot = Path.Combine(StateDirectory, "handoff-evidence");
    }

    public string LocalApplicationData { get; }
    public string StateDirectory { get; }
    public string DefaultExportDirectory { get; }
    public string PreferencesPath { get; }
    public string CaptureProfilePath { get; }
    public string SingleIdentityV3Path { get; }
    public string AgentArtifactsRoot { get; }
    public string AgentReportsRoot { get; }
    public string AgentTransactionStateRoot { get; }
    public string HandoffEvidenceRoot { get; }
    public string ExportDirectory => DefaultExportDirectory;

    public static HardwareSingleStoragePaths ResolveKnownFolder() =>
        Resolve(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData));

    public static HardwareSingleStoragePaths Resolve(string localApplicationData)
    {
        if (string.IsNullOrWhiteSpace(localApplicationData) ||
            !Path.IsPathFullyQualified(localApplicationData))
        {
            throw new InvalidDataException(
                "Windows LocalApplicationData must be an absolute fixed-local path.");
        }

        var root = Path.GetFullPath(localApplicationData);
        EnsureDirectoryPath(root, mustExist: true);
        return new HardwareSingleStoragePaths(root);
    }

    internal void ConfigureAgentStorage(ProcessStartInfo startInfo, string artifactsRoot)
    {
        // Recheck before Process.Start: directory entries can change after resolution.
        // Do not create/migrate/adopt any state here. Only this new child's environment
        // is normalized; native Defaults needs LOCALAPPDATA even before argument parsing.
        try
        {
            EnsureDirectoryPath(LocalApplicationData, mustExist: true);
            EnsureDirectoryPath(artifactsRoot);
            EnsureDirectoryPath(AgentReportsRoot);
            EnsureDirectoryPath(AgentTransactionStateRoot);
        }
        catch (Exception exception) when (exception is InvalidDataException or IOException or UnauthorizedAccessException or
            ArgumentException or NotSupportedException or System.Security.SecurityException)
        {
            throw new HardwareCameraAgentLaunchException(
                "Camera Agentの保存先を安全に確認できないため、起動せずに停止しました。",
                exception,
                requestMayHaveBeenDispatched: false);
        }

        startInfo.ArgumentList.Add("--artifacts-root");
        startInfo.ArgumentList.Add(artifactsRoot);
        startInfo.ArgumentList.Add("--reports-root");
        startInfo.ArgumentList.Add(AgentReportsRoot);
        startInfo.ArgumentList.Add("--transaction-state-root");
        startInfo.ArgumentList.Add(AgentTransactionStateRoot);
        startInfo.Environment["LOCALAPPDATA"] = LocalApplicationData;
    }

    private static void EnsureDirectoryPath(string path, bool mustExist = false)
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
        if (mustExist && !Directory.Exists(path))
        {
            throw new InvalidDataException("Windows LocalApplicationData is unavailable.");
        }
        for (var current = new DirectoryInfo(path); current is not null; current = current.Parent)
        {
            FileAttributes attributes;
            try
            {
                attributes = File.GetAttributes(current.FullName);
            }
            catch (Exception exception) when (exception is FileNotFoundException or DirectoryNotFoundException)
            {
                continue;
            }
            if ((attributes & FileAttributes.Directory) == 0)
            {
                throw new InvalidDataException("A storage directory path is occupied by a regular file.");
            }
        }
    }
}
