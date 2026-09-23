using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.OperatorStatus;
using A0CameraStitcher.M3.OperatorShell;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

internal static class GuiReviewDisplayContracts
{
    internal static async Task RunAsync(string reviewCliPath)
    {
        Check(Path.IsPathFullyQualified(reviewCliPath) && File.Exists(reviewCliPath), "Review CLI is unavailable.");
        var root = Path.Combine(Path.GetTempPath(), "A0-GuiReviewDisplay-" + Guid.NewGuid().ToString("N"));
        var product = Path.Combine(root, "product");
        var adapter = new M2OfflineStitcherProcessAdapter(Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe"));
        var job = Guid.NewGuid();
        var transaction = Guid.NewGuid();
        var originals = new Dictionary<string, string>();
        foreach (var alias in new[] { "CAM-A", "CAM-B" })
        {
            var path = Path.Combine(product, "transactions", transaction.ToString("N"), alias, "original.jpg");
            await adapter.CaptureAsync(alias, transaction, path, CancellationToken.None); // SDK-free synthetic image generator.
            originals.Add(alias, path);
        }
        var jobRoot = Path.Combine(product, "stitch-jobs", job.ToString("N"));
        Directory.CreateDirectory(jobRoot);
        var stitched = Path.Combine(jobRoot, "stitched.jpg");
        File.Copy(originals["CAM-A"], stitched);
        await File.WriteAllTextAsync(Path.Combine(jobRoot, "stitch-job.manifest.json"), JsonSerializer.Serialize(new
        {
            schemaVersion = "a0.stitch-job-manifest.v1", stitchJobId = job.ToString("N"), captureTransactionId = transaction.ToString("N"),
            inputs = originals.Select(pair => new { cameraAlias = pair.Key, sha256 = Hash(pair.Value), encodedSizeBytes = new FileInfo(pair.Value).Length }),
            rigProfile = new { profileId = "synthetic-gui", version = "1.0.0", sha256 = new string('c', 64) },
            engine = new { engineId = "a0.m2.offline-stitcher", version = "1.0.0" },
            output = new { relativePath = "stitched.jpg", sha256 = Hash(stitched), widthPixels = 16, heightPixels = 8,
                encodedSizeBytes = new FileInfo(stitched).Length },
            terminalResultState = "Succeeded", completedAtUtc = "2026-09-23T00:00:00Z", automaticRetryCount = 0,
        }));
        var store = new FileOperatorReviewStore(Path.Combine(product, "operator-review"));
        var pending = new OperatorReviewRecord { ResultId = job.ToString("N"), TransactionId = transaction.ToString("N"),
            ReviewKind = "Simulated", State = "Pending", UpdatedAtUtc = DateTimeOffset.UtcNow };
        await store.SaveAsync(pending);
        var originalHash = Hash(originals["CAM-A"]);
        var outputHash = Hash(stitched);
        var window = new MainWindow(root);
        try
        {
            window.Show();
            var viewModel = (OperatorShellViewModel)window.DataContext;
            await WaitUntilAsync(() => viewModel.CanOpenHistoricalReview, "Synthetic GUI did not reach the history gate.");
            using var process = Process.GetCurrentProcess();
            var instance = FormattableString.Invariant($"{process.Id}-{process.StartTime.ToUniversalTime().Ticks}");
            using var cliReply = await ShowViaCliAsync(reviewCliPath, instance, pending.ResultId, "cam-a");
            Check(cliReply.RootElement.GetProperty("status").GetString() == "ok" &&
                cliReply.RootElement.GetProperty("data").GetProperty("reply").GetProperty("outcome").GetInt32() == 0,
                "CLI did not acknowledge the selected GUI image.");
            var viewer = window.OwnedWindows.OfType<ReviewImageWindow>().SingleOrDefault()
                ?? throw new Exception("Verified image window was not created.");
            Check(viewer.IsVisible && viewer.Title.Contains("採用不可", StringComparison.Ordinal),
                "Verified read-only image was not actually visible.");
            Check(!viewModel.CanOpenHistoricalReview && !viewModel.CanCapture && viewModel.TransactionStartCount == 0,
                "Display did not hold the history gate or started capture.");
            viewer.Close();
            Check(viewModel.CanOpenHistoricalReview, "Closing the viewer did not release the history gate.");
            Check((await store.ReadOneReadOnlyAsync(pending.ResultId)).State == "Pending" &&
                Hash(originals["CAM-A"]) == originalHash && Hash(stitched) == outputHash,
                "Display changed the review or source images.");
            await File.AppendAllTextAsync(originals["CAM-A"], "tampered-test-only");
            var rejected = await OperatorReviewDisplayClient.ShowAsync(instance, pending.ResultId, "cam-a");
            Check(rejected.Outcome == ReviewDisplayOutcome.Unavailable &&
                !window.OwnedWindows.OfType<ReviewImageWindow>().Any(item => item.IsVisible) && viewModel.CanOpenHistoricalReview,
                "Changed original was shown or left the history gate locked.");
        }
        finally
        {
            window.Close();
            await WaitUntilAsync(() => !window.IsVisible, "Synthetic GUI did not close.");
        }
        Console.WriteLine("PASS actual synthetic MainWindow review display, image hash, UI gate, no acceptance/capture, changed-image refusal; hardwareOperations=0");
    }

    private static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant();
    private static async Task<JsonDocument> ShowViaCliAsync(string cliPath, string instance, string resultId, string image)
    {
        var start = new ProcessStartInfo("dotnet") { UseShellExecute = false, RedirectStandardOutput = true,
            RedirectStandardError = true, CreateNoWindow = true };
        foreach (var value in new[] { cliPath, "gui-show-review", "--instance", instance, "--result-id", resultId,
            "--image", image }) start.ArgumentList.Add(value);
        using var child = Process.Start(start) ?? throw new Exception("Review CLI failed to start.");
        var output = child.StandardOutput.ReadToEndAsync();
        var errors = child.StandardError.ReadToEndAsync();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(50));
        await child.WaitForExitAsync(timeout.Token);
        Check(child.ExitCode == 0 && string.IsNullOrWhiteSpace(await errors), "Review CLI failed.");
        return JsonDocument.Parse(await output);
    }
    private static void Check(bool valid, string message) { if (!valid) throw new Exception(message); }
    private static async Task WaitUntilAsync(Func<bool> ready, string message)
    {
        for (var attempt = 0; attempt < 100 && !ready(); attempt++) await Task.Delay(50);
        Check(ready(), message);
    }
}
