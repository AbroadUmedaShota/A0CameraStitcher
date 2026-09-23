using System.Diagnostics;
using System.Globalization;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.Foundation.OperatorStatus;

public enum ReviewDisplayOutcome { Displayed, NotReady, Unavailable }

public sealed record ReviewDisplayRequest(string ResultId, string Image);

public sealed record ReviewDisplayReply(string InstanceId, int ProcessId, long ProcessStartUtcTicks,
    int ContractVersion, ReviewDisplayOutcome Outcome);

/// <summary>Separate, bounded GUI display endpoint. It cannot accept a review or control a camera.</summary>
public sealed partial class OperatorReviewDisplayServer : IAsyncDisposable
{
    internal const string Prefix = "a0.operator.review-display.v1.";
    private readonly Func<ReviewDisplayRequest, CancellationToken, Task<ReviewDisplayOutcome>> _display;
    private readonly CancellationTokenSource _stop = new();
    private readonly int _pid;
    private readonly long _startTicks;
    private int _disposed;

    public OperatorReviewDisplayServer(Func<ReviewDisplayRequest, CancellationToken, Task<ReviewDisplayOutcome>> display)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException();
        _display = display ?? throw new ArgumentNullException(nameof(display));
        using var process = Process.GetCurrentProcess();
        _pid = process.Id;
        _startTicks = process.StartTime.ToUniversalTime().Ticks;
        InstanceId = FormattableString.Invariant($"{_pid}-{_startTicks}");
        Completion = RunAsync();
    }

    public string InstanceId { get; }
    public Task Completion { get; }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _stop.Cancel();
        try { await Completion.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false); }
        catch (OperationCanceledException) { }
        catch (TimeoutException) { }
        finally { _stop.Dispose(); }
    }

    private async Task RunAsync()
    {
        try
        {
            while (!_stop.IsCancellationRequested)
            {
                await using var pipe = new NamedPipeServerStream(Prefix + InstanceId, PipeDirection.InOut, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token);
                try
                {
                    await pipe.WaitForConnectionAsync(_stop.Token).ConfigureAwait(false);
                    timeout.CancelAfter(TimeSpan.FromSeconds(45));
                    using var requester = VerifyClientSession(pipe);
                    var wire = await ReadAsync(pipe, 256, timeout.Token).ConfigureAwait(false);
                    var request = Parse(wire);
                    if (request is null) continue;
                    var outcome = await _display(request, timeout.Token).WaitAsync(timeout.Token).ConfigureAwait(false);
                    var response = JsonSerializer.Serialize(new ReviewDisplayReply(InstanceId, _pid, _startTicks, 1, outcome));
                    await WriteAsync(pipe, response, 512, timeout.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (_stop.IsCancellationRequested) { return; }
                catch (Exception) { /* A failed optional request must not affect the operator GUI. */ }
            }
        }
        catch (Exception) { /* Endpoint construction failure must not crash the GUI. */ }
    }

    private static ReviewDisplayRequest? Parse(string wire)
    {
        using var json = JsonDocument.Parse(wire);
        var root = json.RootElement;
        if (root.ValueKind != JsonValueKind.Object || root.EnumerateObject().Count() != 3 ||
            !root.TryGetProperty("command", out var command) || command.GetString() != "show-review" ||
            !root.TryGetProperty("resultId", out var id) || id.ValueKind != JsonValueKind.String ||
            !root.TryGetProperty("image", out var image) || image.ValueKind != JsonValueKind.String)
            return null;
        var resultId = id.GetString();
        var choice = image.GetString();
        return OperatorReviewDisplayClient.ValidRequest(resultId, choice)
            ? new ReviewDisplayRequest(resultId!, choice!) : null;
    }

    private static Process VerifyClientSession(NamedPipeServerStream pipe)
    {
        if (!GetNamedPipeClientProcessId(pipe.SafePipeHandle, out var pid) || pid > int.MaxValue)
            throw new UnauthorizedAccessException();
        var client = Process.GetProcessById((int)pid);
        try
        {
            _ = client.Handle;
            using var current = Process.GetCurrentProcess();
            if (client.SessionId != current.SessionId) throw new UnauthorizedAccessException();
            return client;
        }
        catch { client.Dispose(); throw; }
    }

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetNamedPipeClientProcessId(SafeHandle pipe, out uint clientProcessId);

    internal static async Task<string> ReadAsync(Stream stream, int maximum, CancellationToken token)
    {
        var header = new byte[4];
        await ReadExactAsync(stream, header, token).ConfigureAwait(false);
        var length = BitConverter.ToInt32(header);
        if (length is <= 0 || length > maximum) throw new InvalidDataException();
        var bytes = new byte[length];
        await ReadExactAsync(stream, bytes, token).ConfigureAwait(false);
        return Encoding.UTF8.GetString(bytes);
    }

    internal static async Task WriteAsync(Stream stream, string wire, int maximum, CancellationToken token)
    {
        var bytes = Encoding.UTF8.GetBytes(wire);
        if (bytes.Length is 0 || bytes.Length > maximum) throw new InvalidDataException();
        await stream.WriteAsync(BitConverter.GetBytes(bytes.Length), token).ConfigureAwait(false);
        await stream.WriteAsync(bytes, token).ConfigureAwait(false);
        await stream.FlushAsync(token).ConfigureAwait(false);
    }

    private static async Task ReadExactAsync(Stream stream, byte[] bytes, CancellationToken token)
    {
        var offset = 0;
        while (offset < bytes.Length)
        {
            var read = await stream.ReadAsync(bytes.AsMemory(offset), token).ConfigureAwait(false);
            if (read == 0) throw new EndOfStreamException();
            offset += read;
        }
    }
}

public static class OperatorReviewDisplayClient
{
    public static async Task<ReviewDisplayReply> ShowAsync(string instanceId, string resultId, string image,
        CancellationToken cancellationToken = default)
    {
        if (!OperatingSystem.IsWindows() || !ValidRequest(resultId, image) ||
            !TryParseInstance(instanceId, out var pid, out var ticks))
            throw new ArgumentException("Invalid review display request.");
        Process target;
        try { target = Process.GetProcessById(pid); }
        catch (ArgumentException error) { throw new IOException("GUI instance is unavailable.", error); }
        using var process = target;
        _ = process.Handle;
        using var current = Process.GetCurrentProcess();
        if (process.SessionId != current.SessionId || process.StartTime.ToUniversalTime().Ticks != ticks)
            throw new InvalidOperationException("GUI instance is unavailable.");
        await using var pipe = new NamedPipeClientStream(".", OperatorReviewDisplayServer.Prefix + instanceId,
            PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(45));
        await pipe.ConnectAsync(timeout.Token).ConfigureAwait(false);
        NamedPipeServerIdentity.Verify(pipe, OperatorReviewDisplayServer.Prefix + instanceId, pid);
        var request = JsonSerializer.Serialize(new { command = "show-review", resultId, image });
        await OperatorReviewDisplayServer.WriteAsync(pipe, request, 256, timeout.Token).ConfigureAwait(false);
        var wire = await OperatorReviewDisplayServer.ReadAsync(pipe, 512, timeout.Token).ConfigureAwait(false);
        var reply = JsonSerializer.Deserialize<ReviewDisplayReply>(wire) ?? throw new InvalidDataException();
        if (reply.InstanceId != instanceId || reply.ProcessId != pid || reply.ProcessStartUtcTicks != ticks ||
            reply.ContractVersion != 1 || !Enum.IsDefined(reply.Outcome)) throw new InvalidDataException();
        return reply;
    }

    internal static bool ValidRequest(string? resultId, string? image) =>
        Guid.TryParseExact(resultId, "N", out var id) && id != Guid.Empty && resultId == id.ToString("N") &&
        image is "stitched" or "cam-a" or "cam-b";

    private static bool TryParseInstance(string? value, out int pid, out long ticks)
    {
        pid = 0; ticks = 0;
        var parts = value?.Split('-');
        return parts is { Length: 2 } && int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out pid) && pid > 0 &&
            long.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out ticks) && ticks > 0 &&
            value == FormattableString.Invariant($"{pid}-{ticks}");
    }
}
