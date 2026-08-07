using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text;

namespace A0CameraStitcher.M3.Foundation;

public sealed class NamedPipeCameraAgentServer : IAsyncDisposable
{
    private readonly string _pipeName;
    private readonly SimulatedCameraAgent _agent;
    private readonly CancellationTokenSource _stopSource = new();
    private Task? _runTask;

    public NamedPipeCameraAgentServer(string pipeName, SimulatedCameraAgent? agent = null)
    {
        if (string.IsNullOrWhiteSpace(pipeName))
        {
            throw new ArgumentException("A pipe name is required.", nameof(pipeName));
        }

        _pipeName = pipeName;
        _agent = agent ?? new SimulatedCameraAgent();
    }

    public void Start()
    {
        if (_runTask is not null)
        {
            throw new InvalidOperationException("The simulated agent server is already started.");
        }

        _runTask = RunAsync(_stopSource.Token);
    }

    public async ValueTask DisposeAsync()
    {
        _stopSource.Cancel();
        if (_runTask is not null)
        {
            try
            {
                await _runTask.ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (_stopSource.IsCancellationRequested)
            {
            }
        }

        _stopSource.Dispose();
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            await using var pipe = new NamedPipeServerStream(
                _pipeName,
                PipeDirection.InOut,
                maxNumberOfServerInstances: 1,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous);

            try
            {
                await pipe.WaitForConnectionAsync(cancellationToken).ConfigureAwait(false);
                await HandleConnectionAsync(pipe, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (IOException) when (!cancellationToken.IsCancellationRequested)
            {
                // A disconnected fake client does not change protocol or camera state.
            }
        }
    }

    private async Task HandleConnectionAsync(Stream stream, CancellationToken cancellationToken)
    {
        var requestJson = await PipeFrameProtocol.ReadAsync(stream, cancellationToken).ConfigureAwait(false);
        CameraAgentResponseEnvelope response;
        try
        {
            var request = CameraAgentProtocolCodec.DeserializeRequest(requestJson);
            response = _agent.Handle(request);
        }
        catch (ProtocolViolationException exception)
        {
            response = CameraAgentProtocolCodec.CreateRejection(
                CameraAgentProtocolCodec.TryExtractRequestId(requestJson),
                exception.ErrorCode);
        }

        await PipeFrameProtocol.WriteAsync(
            stream,
            CameraAgentProtocolCodec.SerializeResponse(response),
            cancellationToken).ConfigureAwait(false);
    }
}

public sealed class NamedPipeCameraAgentClient
{
    private readonly string _pipeName;
    private readonly TimeSpan _connectTimeout;

    public NamedPipeCameraAgentClient(string pipeName, TimeSpan? connectTimeout = null)
    {
        if (string.IsNullOrWhiteSpace(pipeName))
        {
            throw new ArgumentException("A pipe name is required.", nameof(pipeName));
        }

        _pipeName = pipeName;
        _connectTimeout = connectTimeout ?? TimeSpan.FromSeconds(5);
    }

    public async Task<CameraAgentResponseEnvelope> SendAsync(
        CameraAgentRequestEnvelope request,
        CancellationToken cancellationToken = default)
    {
        var responseJson = await SendRawAsync(
            CameraAgentProtocolCodec.SerializeRequest(request),
            cancellationToken).ConfigureAwait(false);
        return CameraAgentProtocolCodec.DeserializeResponse(responseJson);
    }

    public async Task<string> SendRawAsync(string requestJson, CancellationToken cancellationToken = default)
    {
        using var timeoutSource = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutSource.CancelAfter(_connectTimeout);

        await using var pipe = new NamedPipeClientStream(
            serverName: ".",
            _pipeName,
            PipeDirection.InOut,
            PipeOptions.Asynchronous);
        await pipe.ConnectAsync(timeoutSource.Token).ConfigureAwait(false);
        await PipeFrameProtocol.WriteAsync(pipe, requestJson, timeoutSource.Token).ConfigureAwait(false);
        return await PipeFrameProtocol.ReadAsync(pipe, timeoutSource.Token).ConfigureAwait(false);
    }
}

internal static class PipeFrameProtocol
{
    private const int HeaderLength = sizeof(int);
    private const int MaximumPayloadLength = 1024 * 1024;

    public static async Task WriteAsync(Stream stream, string message, CancellationToken cancellationToken)
    {
        var payload = Encoding.UTF8.GetBytes(message);
        if (payload.Length is <= 0 or > MaximumPayloadLength)
        {
            throw new IOException("The pipe payload length is outside the allowed range.");
        }

        var header = new byte[HeaderLength];
        BinaryPrimitives.WriteInt32LittleEndian(header, payload.Length);
        await stream.WriteAsync(header, cancellationToken).ConfigureAwait(false);
        await stream.WriteAsync(payload, cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    public static async Task<string> ReadAsync(Stream stream, CancellationToken cancellationToken)
    {
        var header = new byte[HeaderLength];
        await ReadExactlyAsync(stream, header, cancellationToken).ConfigureAwait(false);
        var payloadLength = BinaryPrimitives.ReadInt32LittleEndian(header);
        if (payloadLength is <= 0 or > MaximumPayloadLength)
        {
            throw new IOException("The pipe payload length is outside the allowed range.");
        }

        var payload = new byte[payloadLength];
        await ReadExactlyAsync(stream, payload, cancellationToken).ConfigureAwait(false);
        return Encoding.UTF8.GetString(payload);
    }

    private static async Task ReadExactlyAsync(
        Stream stream,
        Memory<byte> buffer,
        CancellationToken cancellationToken)
    {
        var read = 0;
        while (read < buffer.Length)
        {
            var count = await stream.ReadAsync(buffer[read..], cancellationToken).ConfigureAwait(false);
            if (count == 0)
            {
                throw new EndOfStreamException("The pipe closed before a complete frame was received.");
            }

            read += count;
        }
    }
}
