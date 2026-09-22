using System.Diagnostics;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation.OperatorStatus;

if (args.Length != 1 || !Path.IsPathFullyQualified(args[0]) || !File.Exists(args[0])) return 2;
var observed = 0;
var expected = new OperatorStatusSnapshot("Simulated", "Review", false, false, false, true, false);
await using var server = new OperatorStatusServer(_ => { Interlocked.Increment(ref observed); return Task.FromResult(expected); });
try
{
    using (var status = await Cli("gui-status", "--instance", server.InstanceId))
    {
        var root = status.RootElement;
        Check(root.GetProperty("version").GetInt32() == 3 && root.GetProperty("status").GetString() == "ok", "CLI status/version");
        var observation = root.GetProperty("data").GetProperty("observation");
        Check(observation.GetProperty("instanceId").GetString() == server.InstanceId, "Wrong instance");
        Check(observation.GetProperty("contractVersion").GetInt32() == 1 &&
            !string.IsNullOrWhiteSpace(observation.GetProperty("build").GetString()), "Server version missing");
        var snapshot = observation.GetProperty("snapshot");
        Check(snapshot.GetProperty("environment").GetString() == "Simulated" && snapshot.GetProperty("uiState").GetString() == "Review" &&
            !snapshot.GetProperty("canCapture").GetBoolean() && snapshot.GetProperty("canOpenHistoricalReview").GetBoolean(), "Wrong GUI gates");
    }
    Check(observed == 1, "One request must observe once");
    // Unsupported wire commands never invoke the observer or shut the endpoint down.
    await using (var pipe = new NamedPipeClientStream(".", "a0.operator.status.v1." + server.InstanceId,
        PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly))
    {
        using var limit = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await pipe.ConnectAsync(limit.Token);
        var wire = Encoding.UTF8.GetBytes("{\"command\":\"capture\"}");
        await pipe.WriteAsync(BitConverter.GetBytes(wire.Length), limit.Token);
        await pipe.WriteAsync(wire, limit.Token);
        var read = await pipe.ReadAsync(new byte[1], limit.Token);
        Check(read == 0, "Unsupported operation produced a response");
    }
    Check(observed == 1 && !server.Completion.IsCompleted, "Unsupported command changed observer/server");
    await using (var oversized = new NamedPipeClientStream(".", "a0.operator.status.v1." + server.InstanceId,
        PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly))
    {
        using var limit = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        await oversized.ConnectAsync(limit.Token);
        await oversized.WriteAsync(BitConverter.GetBytes(257), limit.Token);
        Check(await oversized.ReadAsync(new byte[1], limit.Token) == 0, "Oversized request accepted");
    }
    Check(observed == 1, "Oversized request invoked observer");
    Check((await OperatorStatusClient.ObserveAsync(server.InstanceId)).Snapshot == expected, "Subsequent status refused");
    await Reject(() => OperatorStatusClient.ObserveAsync("0" + server.InstanceId));
    var parts = server.InstanceId.Split('-');
    await Reject(() => OperatorStatusClient.ObserveAsync(parts[0] + "-" + (long.Parse(parts[1]) + 1)));
    using (var rejected = await Cli("capture")) Check(rejected.RootElement.GetProperty("status").GetString() != "ok", "Mutation exposed");
    await server.DisposeAsync();
    Check(server.Completion.IsCompleted, "Dispose did not terminate status server");
    Console.WriteLine("PASS explicit-instance real-pipe CLI status, UI gate values, stale/noncanonical identity refusal, unsupported command isolation, shutdown; hardwareOperations=0; actualGuiAcceptance=notRun");
    return 0;
}
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

static void Check(bool valid, string message) { if (!valid) throw new Exception(message); }
static async Task Reject(Func<Task<OperatorStatusObservation>> call)
{
    try { await call(); } catch (Exception error) when (error is ArgumentException or InvalidOperationException or IOException) { return; }
    throw new Exception("Invalid identity accepted");
}
async Task<JsonDocument> Cli(params string[] arguments)
{
    var start = new ProcessStartInfo("dotnet") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
    start.ArgumentList.Add(args[0]);
    foreach (var argument in arguments) start.ArgumentList.Add(argument);
    using var child = Process.Start(start)!;
    var output = child.StandardOutput.ReadToEndAsync();
    var errors = child.StandardError.ReadToEndAsync();
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
    await child.WaitForExitAsync(timeout.Token);
    Check(child.ExitCode == (arguments[0] == "capture" ? 2 : 0), "Unexpected CLI exit");
    Check(string.IsNullOrWhiteSpace(await errors), "CLI stderr");
    return JsonDocument.Parse(await output);
}
