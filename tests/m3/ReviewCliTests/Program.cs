using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation;

if (args is [var verificationCli, var adapter])
    return await ReviewVerificationContracts.RunAsync(verificationCli, adapter);
if (args.Length != 1 || !Path.IsPathFullyQualified(args[0]) || !File.Exists(args[0])) return 2;
var cli = args[0];
var root = Path.Combine(Path.GetTempPath(), "A0ReviewCli-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
static void Check(bool value, string message) { if (!value) throw new Exception(message); }
static async Task Reject(Func<Task> action)
{
    try { await action(); } catch (IOException) { return; }
    throw new Exception("Expected observation refusal.");
}
static Dictionary<string, string> Snapshot(string directory) => Directory.GetFiles(directory)
    .ToDictionary(path => Path.GetFileName(path), path => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))));
async Task<JsonDocument> Invoke(int expectedExit, params string[] arguments)
{
    var start = new ProcessStartInfo("dotnet") { UseShellExecute = false,
        RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
    start.ArgumentList.Add(cli);
    foreach (var argument in arguments) start.ArgumentList.Add(argument);
    using var process = Process.Start(start) ?? throw new Exception("CLI did not start.");
    var output = process.StandardOutput.ReadToEndAsync();
    var errors = process.StandardError.ReadToEndAsync();
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
    await process.WaitForExitAsync(timeout.Token);
    Check(process.ExitCode == expectedExit, "CLI exit code mismatch.");
    Check(string.IsNullOrWhiteSpace(await errors), "Unexpected CLI stderr.");
    return JsonDocument.Parse(await output);
}
try
{
    var missing = Path.Combine(root, "missing");
    await Reject(() => new FileOperatorReviewStore(missing).QueryReadOnlyAsync());
    Check(!Directory.Exists(missing), "Query created a missing root.");
    var uninitialized = Path.Combine(root, "uninitialized");
    Directory.CreateDirectory(uninitialized);
    await Reject(() => new FileOperatorReviewStore(uninitialized).QueryReadOnlyAsync());
    Check(!Directory.EnumerateFileSystemEntries(uninitialized).Any(), "Query initialized a lock.");
    var empty = Path.Combine(root, "initialized-empty");
    await new FileOperatorReviewStore(empty).LoadAsync(); // Fixture initialization, not the query path.
    using (var result = await Invoke(0, "reviews", "--root", empty))
        Check(result.RootElement.GetProperty("data").GetProperty("records").GetArrayLength() == 0,
            "Successfully observed empty state differs from unavailable.");
    var data = Path.Combine(root, "records");
    var store = new FileOperatorReviewStore(data);
    for (var index = 0; index < 27; index++)
        await store.SaveAsync(new OperatorReviewRecord { ResultId = $"result-{index:D2}",
            TransactionId = $"transaction-{index:D2}", ReviewKind = "Simulated", State = "Pending",
            UpdatedAtUtc = new DateTimeOffset(2026, 9, 22, 0, 0, 0, TimeSpan.Zero).AddSeconds(index) });
    var before = Snapshot(data);
    var first = await store.QueryReadOnlyAsync();
    var second = await store.QueryReadOnlyAsync(first.NextOffset!.Value);
    Check(first.Total == 27 && first.Records.Count == 25 && first.Records[0].ResultId == "result-26" &&
        second.Records.Count == 2 && second.NextOffset is null, "Bounded paging failed.");
    using (var lease = new FileStream(Path.Combine(data, ".review.lock"), FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        await Reject(() => store.QueryReadOnlyAsync());
    using (var result = await Invoke(0, "describe"))
        Check(result.RootElement.GetProperty("status").GetString() == "ok", "Describe failed.");
    using (var result = await Invoke(0, "reviews", "--root", data, "--offset", "25", "--limit", "25"))
        Check(result.RootElement.GetProperty("data").GetProperty("records").GetArrayLength() == 2, "CLI paging failed.");
    using (var result = await Invoke(2, "reviews", "--root", missing))
        Check(result.RootElement.GetProperty("errorCode").GetString() == "observation_unavailable", "Missing differs from empty.");
    using (var result = await Invoke(2, "reviews", "--root", data, "--root", data))
        Check(result.RootElement.GetProperty("errorCode").GetString() == "invalid_input", "Duplicate input not refused.");
    using (var result = await Invoke(2, "accept", "--root", data))
        Check(result.RootElement.GetProperty("errorCode").GetString() == "invalid_input", "Mutation must not be exposed.");
    using (var result = await Invoke(2, "reviews", "--root", data, "--limit", "26"))
        Check(result.RootElement.GetProperty("errorCode").GetString() == "invalid_input", "Page limit must reject.");
    var after = Snapshot(data);
    Check(before.Count == after.Count && before.All(pair => after.GetValueOrDefault(pair.Key) == pair.Value),
        "Queries changed persisted state.");
    var partial = Path.Combine(data, "fixture.partial");
    await File.WriteAllTextAsync(partial, "incomplete fixture");
    using (var result = await Invoke(2, "reviews", "--root", data))
        Check(result.RootElement.GetProperty("errorCode").GetString() == "invalid_metadata", "Partial must not look empty.");
    Check(File.Exists(partial), "Query deleted partial evidence.");
    Console.WriteLine("{\"reviewCliContracts\":\"passed\",\"hardwareOperations\":0}");
    return 0;
}
finally
{
    // Solely the exact fresh synthetic fixture created by this process.
    Directory.Delete(root, recursive: true);
}
