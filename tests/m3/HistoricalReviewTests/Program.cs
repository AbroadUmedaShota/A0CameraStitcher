using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;

if (args.Length != 1 || !Path.IsPathFullyQualified(args[0]) || !File.Exists(args[0])) return 2;
var adapter = new M2OfflineStitcherProcessAdapter(args[0]);
var root = Path.Combine(Path.GetTempPath(), "A0HistoricalReview-" + Guid.NewGuid().ToString("N"));
var transaction = Guid.NewGuid();
var job = Guid.NewGuid();
var jobRoot = Path.Combine(root, "stitch-jobs", job.ToString("N"));
Directory.CreateDirectory(jobRoot);
static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
static void Check(bool value, string message) { if (!value) throw new Exception(message); }
static async Task Reject(Func<Task> action)
{
    try { await action(); }
    catch (Exception error) when (error is IOException or InvalidDataException or ArgumentException or InvalidOperationException or OperationCanceledException)
    { return; }
    throw new Exception("Invalid historical result was accepted.");
}
try
{
    var originals = new Dictionary<string, string>();
    foreach (var alias in new[] { "CAM-A", "CAM-B" })
    {
        var path = Path.Combine(root, "transactions", transaction.ToString("N"), alias, "original.jpg");
        // Explicitly synthetic fixture generation, not hardware capture.
        await adapter.CaptureAsync(alias, transaction, path, CancellationToken.None);
        originals.Add(alias, path);
    }
    var output = Path.Combine(jobRoot, "stitched.jpg");
    File.Copy(originals["CAM-A"], output); // Fixture for verification, not a claim of stitching quality.
    var manifestPath = Path.Combine(jobRoot, "stitch-job.manifest.json");
    var manifest = new
    {
        schemaVersion = "a0.stitch-job-manifest.v1",
        stitchJobId = job.ToString("N"), captureTransactionId = transaction.ToString("N"),
        inputs = originals.Select(pair => new { cameraAlias = pair.Key, sha256 = Hash(pair.Value),
            encodedSizeBytes = new FileInfo(pair.Value).Length }),
        rigProfile = new { profileId = "synthetic-history", version = "1.0.0", sha256 = new string('c', 64) },
        engine = new { engineId = "a0.m2.offline-stitcher", version = "1.0.0" },
        output = new { relativePath = "stitched.jpg", sha256 = Hash(output), widthPixels = 16, heightPixels = 8,
            encodedSizeBytes = new FileInfo(output).Length },
        terminalResultState = "Succeeded", completedAtUtc = "2026-09-22T00:00:00Z", automaticRetryCount = 0,
    };
    await File.WriteAllTextAsync(manifestPath, JsonSerializer.Serialize(manifest));
    var record = new OperatorReviewRecord { ResultId = job.ToString("N"), TransactionId = transaction.ToString("N"),
        ReviewKind = "Simulated", State = "Pending", UpdatedAtUtc = DateTimeOffset.UtcNow };
    var before = Directory.GetFiles(root, "*", SearchOption.AllDirectories).ToDictionary(path => path, Hash);
    var result = await adapter.VerifyHistoricalReviewAsync(root, record);
    Check(result.JobId == job && result.TransactionId == transaction && result.ReviewKind == "Simulated" &&
        result.Originals.Count == 2 && result.Originals.All(item => item.IsCanonicalJpeg && item.Width == 16 && item.Height == 8) &&
        result.StitchedSha256 == Hash(output), "Historical snapshot did not retain verified identities and files.");
    Check(before.All(pair => Hash(pair.Key) == pair.Value) && Directory.GetFiles(root, "*", SearchOption.AllDirectories).Length == before.Count,
        "Read-only verification changed artifacts.");
    var v2 = JsonNode.Parse(JsonSerializer.Serialize(manifest))!.AsObject();
    v2["schemaVersion"] = "a0.stitch-job-manifest.v2";
    v2["seamNavigation"] = new JsonObject { ["coordinateSystem"] = "StitchedOutputPixelCenter.v1",
        ["available"] = true, ["xPixels"] = 6, ["yPixels"] = 3 };
    await File.WriteAllTextAsync(manifestPath, v2.ToJsonString());
    var v2Hash = Hash(manifestPath);
    var v2Result = await adapter.VerifyHistoricalReviewAsync(root, record);
    Check(v2Result.JobId == job && Hash(manifestPath) == v2Hash, "Current v2 verification failed or mutated its manifest.");
    v2["seamNavigation"]!["xPixels"] = 16; // Outside the declared 16-pixel output.
    await File.WriteAllTextAsync(manifestPath, v2.ToJsonString());
    await Reject(() => adapter.VerifyHistoricalReviewAsync(root, record));
    v2["seamNavigation"]!["xPixels"] = 6;
    await File.WriteAllTextAsync(manifestPath, v2.ToJsonString());
    await Reject(() => adapter.VerifyHistoricalReviewAsync(root, record with { State = "Accepted" }));
    await Reject(() => adapter.VerifyHistoricalReviewAsync(root, record with { ResultId = "../other" }));
    await Reject(() => adapter.VerifyHistoricalReviewAsync(root, record with { TransactionId = Guid.NewGuid().ToString("N") }));
    await Reject(() => adapter.VerifyHistoricalReviewAsync(root, record, new CancellationToken(true)));
    var savedA = await File.ReadAllBytesAsync(originals["CAM-A"]);
    await File.AppendAllTextAsync(originals["CAM-A"], "tampered");
    await Reject(() => adapter.VerifyHistoricalReviewAsync(root, record));
    await File.WriteAllBytesAsync(originals["CAM-A"], savedA); // Restore only this synthetic test fixture.
    File.Move(originals["CAM-B"], originals["CAM-B"] + ".fixture");
    await Reject(() => adapter.VerifyHistoricalReviewAsync(root, record));
    File.Move(originals["CAM-B"] + ".fixture", originals["CAM-B"]);
    await File.AppendAllTextAsync(output, "tampered");
    await Reject(() => adapter.VerifyHistoricalReviewAsync(root, record));
    Check(File.Exists(output) && File.Exists(originals["CAM-A"]) && File.Exists(originals["CAM-B"]),
        "Rejected verification deleted evidence.");
    Console.WriteLine("{\"historicalReviewArtifacts\":\"passed\",\"hardwareOperations\":0}");
    return 0;
}
finally
{
    Directory.Delete(root, recursive: true); // Exact fresh synthetic fixture owned by this process.
}
