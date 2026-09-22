using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;

internal static class ReviewVerificationContracts
{
    internal static async Task<int> RunAsync(string cli, string adapterPath)
    {
        if (!Path.IsPathFullyQualified(cli) || !File.Exists(cli) || !Path.IsPathFullyQualified(adapterPath) || !File.Exists(adapterPath)) return 2;
        var fixture = Path.Combine(Path.GetTempPath(), "A0-VerifyReviewCli-" + Guid.NewGuid().ToString("N"));
        var deployment = Path.Combine(fixture, "cli");
        var root = Path.Combine(fixture, "product");
        Directory.CreateDirectory(deployment);
        foreach (var file in Directory.GetFiles(Path.GetDirectoryName(cli)!)) File.Copy(file, Path.Combine(deployment, Path.GetFileName(file)));
        var deployedCli = Path.Combine(deployment, Path.GetFileName(cli));
        var job = Guid.NewGuid();
        var tx = Guid.NewGuid();
        var jobRoot = Path.Combine(root, "stitch-jobs", job.ToString("N"));
        Directory.CreateDirectory(jobRoot);
        var adapter = new M2OfflineStitcherProcessAdapter(adapterPath);
        try
        {
            var originals = new Dictionary<string, string>();
            foreach (var alias in new[] { "CAM-A", "CAM-B" })
            {
                var path = Path.Combine(root, "transactions", tx.ToString("N"), alias, "original.jpg");
                await adapter.CaptureAsync(alias, tx, path, CancellationToken.None); // Synthetic generator, not hardware.
                originals.Add(alias, path);
            }
            var output = Path.Combine(jobRoot, "stitched.jpg");
            File.Copy(originals["CAM-A"], output);
            var manifestPath = Path.Combine(jobRoot, "stitch-job.manifest.json");
            await File.WriteAllTextAsync(manifestPath, JsonSerializer.Serialize(new
            {
                schemaVersion = "a0.stitch-job-manifest.v1", stitchJobId = job.ToString("N"), captureTransactionId = tx.ToString("N"),
                inputs = originals.Select(pair => new { cameraAlias = pair.Key, sha256 = Hash(pair.Value), encodedSizeBytes = new FileInfo(pair.Value).Length }),
                rigProfile = new { profileId = "synthetic-cli", version = "1.0.0", sha256 = new string('c', 64) },
                engine = new { engineId = "a0.m2.offline-stitcher", version = "1.0.0" },
                output = new { relativePath = "stitched.jpg", sha256 = Hash(output), widthPixels = 16, heightPixels = 8, encodedSizeBytes = new FileInfo(output).Length },
                terminalResultState = "Succeeded", completedAtUtc = "2026-09-22T00:00:00Z", automaticRetryCount = 0,
            }));
            var record = new OperatorReviewRecord { ResultId = job.ToString("N"), TransactionId = tx.ToString("N"),
                ReviewKind = "Simulated", State = "Pending", UpdatedAtUtc = DateTimeOffset.UtcNow };
            var store = new FileOperatorReviewStore(Path.Combine(root, "operator-review"));
            await store.SaveAsync(record);
            string[] Valid(string? kind = null, string? id = null) => ["verify-review", "--product-root", root,
                "--result-id", id ?? record.ResultId, "--expected-kind", kind ?? "Simulated"];
            var before = Snapshot(root);
            using (var missingAdapter = await Invoke(deployedCli, 2, Valid()))
                Check(missingAdapter.RootElement.GetProperty("errorCode").GetString() == "observation_unavailable", "Missing adapter was misclassified.");
            File.Copy(adapterPath, Path.Combine(deployment, "A0CameraStitcher.M2Adapter.exe"), overwrite: true);
            using (var description = await Invoke(deployedCli, 0, "describe"))
                Check(description.RootElement.GetProperty("version").GetInt32() == 2 &&
                    description.RootElement.GetProperty("data").GetProperty("operations").EnumerateArray().Any(item => item.GetString() == "verify-review"),
                    "Verification was not discoverable in the versioned contract.");
            using (var verified = await Invoke(deployedCli, 0, Valid()))
            {
                var data = verified.RootElement.GetProperty("data");
                Check(data.GetProperty("artifactVerification").GetString() == "verified" &&
                    data.GetProperty("manifestSha256").GetString() == Hash(manifestPath) &&
                    data.GetProperty("resultId").GetString() == record.ResultId &&
                    data.GetProperty("transactionId").GetString() == record.TransactionId &&
                    data.GetProperty("reviewKind").GetString() == "Simulated" &&
                    data.GetProperty("adapterSha256").GetString()!.Equals(Hash(adapterPath), StringComparison.OrdinalIgnoreCase), "Wrong verification identity or evidence.");
                var stitched = data.GetProperty("output");
                Check(stitched.GetProperty("relativePath").GetString() == $"stitch-jobs/{job:N}/stitched.jpg" &&
                    stitched.GetProperty("sha256").GetString() == Hash(output) && stitched.GetProperty("width").GetInt32() == 16 &&
                    stitched.GetProperty("height").GetInt32() == 8, "Output artifact reference is incorrect.");
                var inputs = data.GetProperty("originals").EnumerateArray().ToArray();
                Check(inputs.Length == 2 && inputs.Select(item => item.GetProperty("alias").GetString()).Distinct().Count() == 2 &&
                    inputs.All(item => originals.TryGetValue(item.GetProperty("alias").GetString()!, out var path) &&
                        item.GetProperty("sha256").GetString() == Hash(path) && item.GetProperty("size").GetInt64() == new FileInfo(path).Length),
                    "Original artifact references are incomplete or incorrect.");
                Check(!Strings(verified.RootElement).Any(value => value.Contains(fixture, StringComparison.OrdinalIgnoreCase)), "CLI exposed an absolute private path.");
            }
            foreach (var arguments in new[] { Valid("Product"), Valid(id: Guid.NewGuid().ToString("N")),
                Valid().Concat(new[] { "--result-id", record.ResultId }).ToArray(),
                Valid().Concat(new[] { "--adapter", adapterPath }).ToArray(), new[] { "accept", "--product-root", root } })
                using (await Invoke(deployedCli, 2, arguments)) { }
            Check(Same(before, Snapshot(root)), "Read-only CLI changed product artifacts or metadata.");
            var metadataPath = Directory.GetFiles(Path.Combine(root, "operator-review"), "*.json").Single();
            var metadataBytes = File.ReadAllBytes(metadataPath);
            await File.WriteAllTextAsync(metadataPath, "{broken");
            using (var invalid = await Invoke(deployedCli, 2, Valid()))
                Check(invalid.RootElement.GetProperty("errorCode").GetString() == "invalid_metadata", "Malformed metadata was misclassified.");
            await File.WriteAllBytesAsync(metadataPath, metadataBytes);
            File.Move(originals["CAM-B"], originals["CAM-B"] + ".fixture");
            using (var missingImage = await Invoke(deployedCli, 2, Valid()))
                Check(missingImage.RootElement.GetProperty("errorCode").GetString() == "invalid_artifacts", "Missing original was not distinguished from an unavailable adapter.");
            File.Move(originals["CAM-B"] + ".fixture", originals["CAM-B"]);
            var originalBytes = File.ReadAllBytes(originals["CAM-A"]);
            await File.AppendAllTextAsync(originals["CAM-A"], "tampered");
            using (await Invoke(deployedCli, 2, Valid())) { }
            Check(File.Exists(originals["CAM-A"]) && File.Exists(output), "Failure deleted evidence.");
            await File.WriteAllBytesAsync(originals["CAM-A"], originalBytes);
            await store.SaveAsync(record with { State = "Accepted", UpdatedAtUtc = DateTimeOffset.UtcNow });
            using (await Invoke(deployedCli, 2, Valid())) { }
            Console.WriteLine("PASS review CLI verification via native adapter, discovery, identity/mode isolation, read-only evidence and mutation refusal; hardwareOperations=0");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine($"FAIL review CLI verification: {error}"); return 1; }
    }

    private static async Task<JsonDocument> Invoke(string cli, int expectedExit, params string[] arguments)
    {
        var start = new ProcessStartInfo("dotnet") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
        start.ArgumentList.Add(cli);
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        using var process = Process.Start(start) ?? throw new Exception("CLI failed to start.");
        var output = process.StandardOutput.ReadToEndAsync();
        var errors = process.StandardError.ReadToEndAsync();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(50));
        await process.WaitForExitAsync(timeout.Token);
        var text = await output;
        Check(process.ExitCode == expectedExit, $"CLI exit mismatch: {text}");
        Check(string.IsNullOrWhiteSpace(await errors), "Unexpected CLI stderr.");
        return JsonDocument.Parse(text);
    }
    private static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
    private static IEnumerable<string> Strings(JsonElement element) => element.ValueKind switch
    {
        JsonValueKind.String => [element.GetString()!],
        JsonValueKind.Object => element.EnumerateObject().SelectMany(property => Strings(property.Value)),
        JsonValueKind.Array => element.EnumerateArray().SelectMany(Strings),
        _ => [],
    };
    private static Dictionary<string, string> Snapshot(string root) => Directory.GetFiles(root, "*", SearchOption.AllDirectories).ToDictionary(path => path, Hash);
    private static bool Same(Dictionary<string, string> before, Dictionary<string, string> after) => before.Count == after.Count && before.All(pair => after.GetValueOrDefault(pair.Key) == pair.Value);
    private static void Check(bool condition, string message) { if (!condition) throw new Exception(message); }
}
