using System.Security.Cryptography;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;

internal static class HistoricalAcceptanceContracts
{
    internal static async Task<int> RunAsync(string executable)
    {
        var adapter = new M2OfflineStitcherProcessAdapter(executable);
        var root = Path.Combine(Path.GetTempPath(), "A0-HistoryAcceptance-" + Guid.NewGuid().ToString("N"));
        var job = Guid.NewGuid();
        var tx = Guid.NewGuid();
        var jobRoot = Path.Combine(root, "stitch-jobs", job.ToString("N"));
        Directory.CreateDirectory(jobRoot);
        try
        {
            var originals = new Dictionary<string, string>();
            foreach (var alias in new[] { "CAM-A", "CAM-B" })
            {
                var path = Path.Combine(root, "transactions", tx.ToString("N"), alias, "original.jpg");
                await adapter.CaptureAsync(alias, tx, path, CancellationToken.None); // Synthetic fixture only.
                originals.Add(alias, path);
            }
            var output = Path.Combine(jobRoot, "stitched.jpg");
            File.Copy(originals["CAM-A"], output); // Verification fixture, not stitch-quality evidence.
            var manifestPath = Path.Combine(jobRoot, "stitch-job.manifest.json");
            var manifest = JsonSerializer.Serialize(new
            {
                schemaVersion = "a0.stitch-job-manifest.v2", stitchJobId = job.ToString("N"), captureTransactionId = tx.ToString("N"),
                inputs = originals.Select(pair => new { cameraAlias = pair.Key, sha256 = Hash(pair.Value), encodedSizeBytes = new FileInfo(pair.Value).Length }),
                rigProfile = new { profileId = "synthetic-history", version = "1.0.0", sha256 = new string('c', 64) },
                engine = new { engineId = "a0.m2.offline-stitcher", version = "1.0.0" },
                output = new { relativePath = "stitched.jpg", sha256 = Hash(output), widthPixels = 16, heightPixels = 8, encodedSizeBytes = new FileInfo(output).Length },
                seamNavigation = new { coordinateSystem = "StitchedOutputPixelCenter.v1", available = true, xPixels = 6, yPixels = 3 },
                terminalResultState = "Succeeded", completedAtUtc = "2026-09-22T00:00:00Z", automaticRetryCount = 0,
            });
            await File.WriteAllTextAsync(manifestPath, manifest);
            var pending = new OperatorReviewRecord { ResultId = job.ToString("N"), TransactionId = tx.ToString("N"),
                ReviewKind = "Simulated", State = "Pending", UpdatedAtUtc = DateTimeOffset.UtcNow.AddMinutes(-1) };
            var shown = await adapter.VerifyHistoricalReviewAsync(root, pending);
            Check(shown.ManifestSha256 == Hash(manifestPath), "Displayed manifest was not fingerprinted.");
            await Reject(() => adapter.AcceptHistoricalReviewAsync(root, pending, shown));
            Check(!Directory.Exists(Path.Combine(root, "operator-review")), "Acceptance created missing metadata storage.");
            var store = new FileOperatorReviewStore(Path.Combine(root, "operator-review"));
            await store.SaveAsync(pending);

            using (var locked = await HistoricalReviewArtifactsVerifier.VerifyLockedAsync(adapter, root, pending, CancellationToken.None))
            {
                foreach (var path in originals.Values.Append(output).Append(manifestPath))
                {
                    try { using var writer = new FileStream(path, FileMode.Open, FileAccess.Write, FileShare.ReadWrite); throw new Exception("Verified artifact was writable."); }
                    catch (IOException) { }
                }
                Check(locked.Artifacts.ManifestSha256 == shown.ManifestSha256, "Locked verifier changed identity.");
            }
            File.Move(manifestPath, manifestPath + ".fixture");
            await Reject(() => adapter.VerifyHistoricalReviewAsync(root, pending));
            using (var exclusive = new FileStream(output, FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }
            File.Move(manifestPath + ".fixture", manifestPath);

            await Reject(() => adapter.AcceptHistoricalReviewAsync(root, pending, shown, new CancellationToken(true)));
            await Reject(() => adapter.AcceptHistoricalReviewAsync(root, pending, shown with { ManifestSha256 = new string('0', 64) }));
            await File.WriteAllTextAsync(manifestPath, manifest + " "); // Valid but not the manifest the operator saw.
            await Reject(() => adapter.AcceptHistoricalReviewAsync(root, pending, shown));
            await File.WriteAllTextAsync(manifestPath, manifest);
            var bytes = await File.ReadAllBytesAsync(originals["CAM-A"]);
            await File.AppendAllTextAsync(originals["CAM-A"], "changed");
            await Reject(() => adapter.AcceptHistoricalReviewAsync(root, pending, shown));
            await File.WriteAllBytesAsync(originals["CAM-A"], bytes);
            var partial = Path.Combine(root, "operator-review", "synthetic.partial");
            await File.WriteAllTextAsync(partial, "incomplete");
            await Reject(() => adapter.AcceptHistoricalReviewAsync(root, pending, shown));
            File.Move(partial, partial + ".fixture"); // Preserve the deliberately injected fixture.
            var updated = pending with { UpdatedAtUtc = pending.UpdatedAtUtc.AddSeconds(1) };
            await store.SaveAsync(updated);
            await Reject(() => adapter.AcceptHistoricalReviewAsync(root, pending, shown));
            Check((await store.QueryReadOnlyAsync()).Records.Single() == updated, "Rejected acceptance changed Pending metadata.");
            var hashes = originals.Values.Append(output).Append(manifestPath).ToDictionary(path => path, Hash);
            var accepted = await adapter.AcceptHistoricalReviewAsync(root, updated, shown);
            Check(accepted.State == "Accepted" && accepted.ResultId == updated.ResultId && accepted.TransactionId == updated.TransactionId &&
                (await store.QueryReadOnlyAsync()).Records.Single() == accepted, "Acceptance was not durably verified.");
            await Reject(() => adapter.AcceptHistoricalReviewAsync(root, updated, shown));
            Check((await store.QueryReadOnlyAsync()).Records.Single() == accepted && hashes.All(pair => Hash(pair.Key) == pair.Value),
                "Duplicate acceptance or original preservation failed.");
            Console.WriteLine("PASS historical acceptance native verification, locks, stale/cancel/partial rejection and durable original-preserving save; hardwareOperations=0");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine($"FAIL historical acceptance: {error}"); return 1; }
        // Fresh synthetic fixtures are retained for diagnosis; no product data is touched.
    }
    private static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
    private static void Check(bool value, string message) { if (!value) throw new Exception(message); }
    private static async Task Reject(Func<Task> action)
    {
        try { await action(); }
        catch (Exception error) when (error is IOException or InvalidDataException or ArgumentException or InvalidOperationException or OperationCanceledException) { return; }
        throw new Exception("Invalid acceptance succeeded.");
    }
}
