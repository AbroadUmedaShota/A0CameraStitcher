using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.Foundation.OperatorStatus;

public sealed record OperatorStatusSnapshot(
    string Environment,
    string UiState,
    bool IsBusy,
    bool IsLiveViewActive,
    bool CanCapture,
    bool CanOpenHistoricalReview,
    bool ShutdownStarted);

public sealed record OperatorStatusObservation(
    string InstanceId,
    int ProcessId,
    long ProcessStartUtcTicks,
    DateTimeOffset ObservedAtUtc,
    OperatorStatusSnapshot Snapshot,
    int ContractVersion,
    string Build);

/// <summary>Read-only local status endpoint. It has no camera, file, or UI mutation command.</summary>
public sealed partial class OperatorStatusServer : IAsyncDisposable
{
    private const string Prefix = "a0.operator.status.v1.";
    private readonly Func<CancellationToken, Task<OperatorStatusSnapshot>> _observe;
    private readonly CancellationTokenSource _stop = new();
    private readonly int _processId;
    private readonly long _processStartUtcTicks;
    private int _disposed;

    public OperatorStatusServer(Func<CancellationToken, Task<OperatorStatusSnapshot>> observe)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException();
        _observe = observe ?? throw new ArgumentNullException(nameof(observe));
        using var process = Process.GetCurrentProcess();
        _processId = process.Id;
        _processStartUtcTicks = process.StartTime.ToUniversalTime().Ticks;
        InstanceId = FormattableString.Invariant($"{_processId}-{_processStartUtcTicks}");
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
                    timeout.CancelAfter(TimeSpan.FromSeconds(5));
                    using var requester = VerifyClientSession(pipe);
                    var request = await ReadAsync(pipe, 256, timeout.Token).ConfigureAwait(false);
                    if (request != "{\"command\":\"status\"}") continue;
                    var snapshot = await _observe(timeout.Token).WaitAsync(timeout.Token).ConfigureAwait(false);
                    var response = JsonSerializer.Serialize(new OperatorStatusObservation(InstanceId, _processId,
                        _processStartUtcTicks, DateTimeOffset.UtcNow, snapshot, 1,
                        typeof(OperatorStatusServer).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "unavailable"));
                    await WriteAsync(pipe, response, 4096, timeout.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (_stop.IsCancellationRequested) { return; }
                catch (Exception) { /* Status is optional; never propagate into the GUI process. */ }
            }
        }
        catch (Exception) { /* Pipe construction failures must not crash the operator UI. */ }
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

    private static async Task<string> ReadAsync(Stream stream, int maximum, CancellationToken token)
    {
        var lengthBytes = new byte[4];
        await ReadExactAsync(stream, lengthBytes, token).ConfigureAwait(false);
        var length = BitConverter.ToInt32(lengthBytes);
        if (length <= 0 || length > maximum) throw new InvalidDataException();
        var bytes = new byte[length];
        await ReadExactAsync(stream, bytes, token).ConfigureAwait(false);
        return Encoding.UTF8.GetString(bytes);
    }

    private static async Task WriteAsync(Stream stream, string value, int maximum, CancellationToken token)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        if (bytes.Length == 0 || bytes.Length > maximum) throw new InvalidDataException();
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

public static class OperatorStatusClient
{
    private const string Prefix = "a0.operator.status.v1.";

    public static async Task<OperatorStatusObservation> ObserveAsync(string instanceId, CancellationToken cancellationToken = default)
    {
        if (!OperatingSystem.IsWindows() || !TryParse(instanceId, out var pid, out var startTicks)) throw new ArgumentException("Invalid status instance.", nameof(instanceId));
        Process target;
        try { target = Process.GetProcessById(pid); }
        catch (ArgumentException error) { throw new IOException("Status instance is unavailable.", error); }
        using var process = target;
        _ = process.Handle; // Retain the OS process identity until after response validation.
        using var current = Process.GetCurrentProcess();
        if (process.SessionId != current.SessionId || process.StartTime.ToUniversalTime().Ticks != startTicks)
            throw new InvalidOperationException("Status instance is unavailable.");
        await using var pipe = new NamedPipeClientStream(".", Prefix + instanceId, PipeDirection.InOut,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(5));
        await pipe.ConnectAsync(timeout.Token).ConfigureAwait(false);
        NamedPipeServerIdentity.Verify(pipe, Prefix + instanceId, pid);
        await WriteAsync(pipe, "{\"command\":\"status\"}", timeout.Token).ConfigureAwait(false);
        var wire = await ReadAsync(pipe, timeout.Token).ConfigureAwait(false);
        var observation = JsonSerializer.Deserialize<OperatorStatusObservation>(wire) ?? throw new InvalidDataException();
        if (observation.InstanceId != instanceId || observation.ProcessId != pid || observation.ProcessStartUtcTicks != startTicks ||
            observation.ContractVersion != 1 || observation.Snapshot is null || string.IsNullOrWhiteSpace(observation.Build))
            throw new InvalidDataException();
        return observation;
    }

    private static bool TryParse(string value, out int pid, out long ticks)
    {
        pid = 0; ticks = 0;
        var parts = value?.Split('-');
        return parts is { Length: 2 } && int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out pid) && pid > 0 &&
            long.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out ticks) && ticks > 0 &&
            value == FormattableString.Invariant($"{pid}-{ticks}");
    }
    private static async Task WriteAsync(Stream stream, string text, CancellationToken token)
    {
        var bytes = Encoding.UTF8.GetBytes(text);
        await stream.WriteAsync(BitConverter.GetBytes(bytes.Length), token).ConfigureAwait(false);
        await stream.WriteAsync(bytes, token).ConfigureAwait(false);
        await stream.FlushAsync(token).ConfigureAwait(false);
    }
    private static async Task<string> ReadAsync(Stream stream, CancellationToken token)
    {
        var header = new byte[4]; await ReadExactAsync(stream, header, token).ConfigureAwait(false);
        var length = BitConverter.ToInt32(header);
        if (length is <= 0 or > 4096) throw new InvalidDataException();
        var bytes = new byte[length]; await ReadExactAsync(stream, bytes, token).ConfigureAwait(false);
        return Encoding.UTF8.GetString(bytes);
    }
    private static async Task ReadExactAsync(Stream stream, byte[] bytes, CancellationToken token)
    {
        var offset = 0; while (offset < bytes.Length) { var read = await stream.ReadAsync(bytes.AsMemory(offset), token).ConfigureAwait(false); if (read == 0) throw new EndOfStreamException(); offset += read; }
    }
}
