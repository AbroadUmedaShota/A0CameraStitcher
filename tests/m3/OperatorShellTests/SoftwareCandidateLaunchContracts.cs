using System.IO;
using A0CameraStitcher.M3.OperatorShell;
using A0CameraStitcher.M3.OperatorShell.Hardware;

internal static class SoftwareCandidateLaunchContracts
{
    public static int Run(string? actualAgentPath = null)
    {
        var root = Path.Combine(Path.GetTempPath(), "A0-SoftwareCandidate-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var app = Path.Combine(root, "app");
            Directory.CreateDirectory(app);
            var agent = Path.Combine(app, "A0CameraStitcher.CameraAgent.exe");
            File.WriteAllBytes(agent, [0x4d, 0x5a]);
            var manifest = Path.Combine(root, "candidate.manifest.json");

            File.WriteAllText(manifest, """
                {"schemaVersion":1,"status":"local-software-candidate","sdkIncluded":false,"files":[{"path":"app/A0CameraStitcher.CameraAgent.exe"}]}
                """);
            var softwareOnly = HardwareSingleCandidateManifest.Inspect(agent);
            Require(softwareOnly.ManifestStatus == HardwareSingleCandidateManifestStatus.SoftwareOnly &&
                !softwareOnly.CanStartHardware &&
                softwareOnly.AvailabilityText == "SDK非同梱のソフトウェア確認用候補。カメラ未照会",
                "A software-only candidate was not blocked.");
            RequireThrows(() => ApplicationLaunchOptions.Parse(["--hardware-single"], app),
                "Direct hardware-single launch bypassed the software-only candidate gate.");

            File.WriteAllText(manifest,
                "{\"schemaVersion\":1,\"status\":\"local-software-candidate\",\"sdkIncluded\":true,\"files\":[]}");
            var declared = HardwareSingleCandidateManifest.Inspect(agent);
            Require(declared.ManifestStatus == HardwareSingleCandidateManifestStatus.SdkInclusionDeclared &&
                declared.CanStartHardware &&
                declared.AvailabilityText == "実行ファイルあり（SDK・実機接続は未確認）",
                "SDK declaration was treated as a hardware-ready claim or blocked the fail-closed readiness screen.");
            Require(ApplicationLaunchOptions.Parse(["--hardware-single"], app).Mode == ApplicationLaunchMode.HardwareSingle,
                "Direct hardware-single launch did not preserve the existing readiness-screen entry for an unverified SDK declaration.");

            File.WriteAllText(manifest,
                "{\"schemaVersion\":1,\"schemaVersion\":1,\"status\":\"local-software-candidate\",\"sdkIncluded\":false}");
            var duplicate = HardwareSingleCandidateManifest.Inspect(agent);
            Require(duplicate.ManifestStatus == HardwareSingleCandidateManifestStatus.Invalid && !duplicate.CanStartHardware,
                "A manifest with duplicate required keys was accepted.");

            File.WriteAllText(manifest,
                "{\"schemaVersion\":2147483648,\"status\":\"local-software-candidate\",\"sdkIncluded\":false}");
            var overflow = HardwareSingleCandidateManifest.Inspect(agent);
            Require(overflow.ManifestStatus == HardwareSingleCandidateManifestStatus.Invalid && !overflow.CanStartHardware,
                "An overflowing required schema version was accepted.");

            File.WriteAllText(manifest, "not-json");
            var malformed = HardwareSingleCandidateManifest.Inspect(agent);
            Require(malformed.ManifestStatus == HardwareSingleCandidateManifestStatus.Invalid && !malformed.CanStartHardware,
                "A malformed manifest was accepted.");

            File.WriteAllText(manifest, new string(' ', (64 * 1024) + 1));
            var oversized = HardwareSingleCandidateManifest.Inspect(agent);
            Require(oversized.ManifestStatus == HardwareSingleCandidateManifestStatus.Invalid && !oversized.CanStartHardware,
                "An oversized manifest was accepted.");

            File.Delete(manifest);
            var absent = HardwareSingleCandidateManifest.Inspect(agent);
            Require(absent.ManifestStatus == HardwareSingleCandidateManifestStatus.Missing && absent.CanStartHardware &&
                absent.AvailabilityText == "実行ファイルあり（SDK・実機接続は未確認）",
                "A missing manifest changed into a hardware-ready claim or stopped the readiness screen.");

            if (!string.IsNullOrWhiteSpace(actualAgentPath))
            {
                var packaged = HardwareSingleCandidateManifest.Inspect(actualAgentPath);
                Require(packaged.ManifestStatus == HardwareSingleCandidateManifestStatus.SoftwareOnly && !packaged.CanStartHardware,
                    "The supplied local software candidate was not recognized as software-only.");
            }

            return 0;
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private static void RequireThrows(Action action, string message)
    {
        try
        {
            action();
        }
        catch (ArgumentException)
        {
            return;
        }

        throw new InvalidOperationException(message);
    }
}
