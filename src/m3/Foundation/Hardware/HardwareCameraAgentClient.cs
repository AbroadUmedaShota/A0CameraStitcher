using System.IO.Pipes;
using System.Buffers.Binary;
using System.Text;

namespace A0CameraStitcher.M3.Foundation.Hardware;

public interface IHardwareCameraAgentTransport
{
    Task<string> SendAsync(string requestJson, CancellationToken cancellationToken = default);
}

public sealed class HardwareCameraAgentConnectException : IOException
{
    internal HardwareCameraAgentConnectException(
        string pipeName,
        bool callerCancellationRequested,
        Exception innerException)
        : base("The hardware Camera Agent pipe connection failed before request dispatch.", innerException)
    {
        PipeName = pipeName;
        CallerCancellationRequested = callerCancellationRequested;
    }

    public string PipeName { get; }

    public bool CallerCancellationRequested { get; }
}

public enum HardwarePipeResponseFailureStage
{
    BeforeResponse,
    PartialHeader,
    PartialBody,
}

public sealed class HardwareCameraAgentIncompleteResponseException : EndOfStreamException
{
    internal HardwareCameraAgentIncompleteResponseException(
        HardwarePipeResponseFailureStage failureStage)
        : base("The hardware pipe closed before a complete frame was received.")
    {
        FailureStage = failureStage;
    }

    public HardwarePipeResponseFailureStage FailureStage { get; }
}

public sealed class NamedPipeHardwareCameraAgentTransport : IHardwareCameraAgentTransport
{
    private const byte DeliveryAcknowledgment = 0x06;
    private readonly string _pipeName;
    private readonly TimeSpan _connectTimeout;
    private readonly TimeSpan _responseTimeout;

    public NamedPipeHardwareCameraAgentTransport(
        string pipeName = HardwareCameraAgentProtocol.DefaultPipeName,
        TimeSpan? connectTimeout = null,
        TimeSpan? responseTimeout = null)
    {
        ValidatePipeName(pipeName);
        _pipeName = pipeName;
        _connectTimeout = ValidateTimeout(
            connectTimeout ?? TimeSpan.FromSeconds(5),
            nameof(connectTimeout));
        _responseTimeout = ValidateTimeout(
            responseTimeout ?? TimeSpan.FromMinutes(4),
            nameof(responseTimeout));
    }

    public async Task<string> SendAsync(
        string requestJson,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(requestJson);
        // GitHub Issue #85: CurrentUserOnly により、別ユーザーが先回りして同名パイプを
        // 作成する named pipe squatting を遮断する(クライアントはサーバ側パイプの所有者が
        // 現在のユーザーであることを検証してから接続する)。正規の Agent は同一ユーザーの
        // 子プロセスがパイプを作成するため影響しない。
        await using var pipe = new NamedPipeClientStream(
            serverName: ".",
            _pipeName,
            PipeDirection.InOut,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);

        using (var connectSource = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken))
        {
            connectSource.CancelAfter(_connectTimeout);
            try
            {
                await pipe.ConnectAsync(connectSource.Token).ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                throw new HardwareCameraAgentConnectException(
                    _pipeName,
                    cancellationToken.IsCancellationRequested,
                    exception);
            }
        }

        using var responseSource = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        responseSource.CancelAfter(_responseTimeout);
        await HardwarePipeFrameProtocol.WriteAsync(pipe, requestJson, responseSource.Token).ConfigureAwait(false);
        var response = await HardwarePipeFrameProtocol.ReadAsync(pipe, responseSource.Token).ConfigureAwait(false);
        await pipe.WriteAsync(new[] { DeliveryAcknowledgment }, responseSource.Token).ConfigureAwait(false);
        await pipe.FlushAsync(responseSource.Token).ConfigureAwait(false);
        return response;
    }

    private static void ValidatePipeName(string pipeName)
    {
        if (string.IsNullOrEmpty(pipeName) || pipeName.Length > 120 || !pipeName.All(character =>
                IsAsciiLetter(character) || IsAsciiDigit(character) || character is '.' or '-' or '_'))
        {
            throw new ArgumentException("The hardware Camera Agent pipe name is invalid.", nameof(pipeName));
        }
    }

    private static TimeSpan ValidateTimeout(TimeSpan value, string parameterName)
    {
        if (value <= TimeSpan.Zero || value > TimeSpan.FromHours(1))
        {
            throw new ArgumentOutOfRangeException(parameterName, "The timeout is outside the allowed range.");
        }

        return value;
    }

    private static bool IsAsciiDigit(char character) => character is >= '0' and <= '9';

    private static bool IsAsciiLetter(char character) =>
        character is >= 'a' and <= 'z' or >= 'A' and <= 'Z';
}

internal static class HardwarePipeFrameProtocol
{
    private const int HeaderLength = sizeof(int);
    private const int MaximumPayloadLength = 1024 * 1024;
    private static readonly UTF8Encoding StrictUtf8 = new(
        encoderShouldEmitUTF8Identifier: false,
        throwOnInvalidBytes: true);

    public static async Task WriteAsync(
        Stream stream,
        string message,
        CancellationToken cancellationToken)
    {
        byte[] payload;
        try
        {
            payload = StrictUtf8.GetBytes(message);
        }
        catch (EncoderFallbackException exception)
        {
            throw new IOException("The hardware pipe payload is not valid UTF-8.", exception);
        }

        if (payload.Length is <= 0 or > MaximumPayloadLength)
        {
            throw new IOException("The hardware pipe payload length is outside the allowed range.");
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
        await ReadExactlyAsync(
            stream, header, readingHeader: true, cancellationToken).ConfigureAwait(false);
        var payloadLength = BinaryPrimitives.ReadInt32LittleEndian(header);
        if (payloadLength is <= 0 or > MaximumPayloadLength)
        {
            throw new IOException("The hardware pipe payload length is outside the allowed range.");
        }

        var payload = new byte[payloadLength];
        await ReadExactlyAsync(
            stream, payload, readingHeader: false, cancellationToken).ConfigureAwait(false);
        try
        {
            return StrictUtf8.GetString(payload);
        }
        catch (DecoderFallbackException exception)
        {
            throw new IOException("The hardware pipe response is not valid UTF-8.", exception);
        }
    }

    private static async Task ReadExactlyAsync(
        Stream stream,
        Memory<byte> buffer,
        bool readingHeader,
        CancellationToken cancellationToken)
    {
        var offset = 0;
        while (offset < buffer.Length)
        {
            var count = await stream.ReadAsync(buffer[offset..], cancellationToken).ConfigureAwait(false);
            if (count == 0)
            {
                var failureStage = readingHeader
                    ? offset == 0
                        ? HardwarePipeResponseFailureStage.BeforeResponse
                        : HardwarePipeResponseFailureStage.PartialHeader
                    : HardwarePipeResponseFailureStage.PartialBody;
                throw new HardwareCameraAgentIncompleteResponseException(failureStage);
            }

            offset += count;
        }
    }
}

public interface IHardwareCameraAgentClient
{
    Task<HardwareCameraAgentReply<HardwareSingleReadinessResult>> GetSingleReadinessAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> CaptureSingleAsync(
        string transactionId,
        string cameraAlias,
        HardwareCaptureSafetyConfirmations confirmations,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> CaptureSingleAsync(
        string transactionId,
        string cameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        HardwareCaptureSafetyConfirmations confirmations,
        bool liveViewHandoffRequested,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareSingleLiveViewResult>> ProbeLiveViewAsync(
        string cameraAlias,
        HardwareLiveViewSafetyConfirmation confirmation,
        int frames = 1,
        int intervalMs = 100,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool expectedLiveViewHandoffRequested,
        CancellationToken cancellationToken = default);
}

public sealed class HardwareCameraAgentClient : IHardwareCameraAgentClient
{
    private readonly IHardwareCameraAgentTransport _transport;

    public HardwareCameraAgentClient(IHardwareCameraAgentTransport transport)
    {
        _transport = transport ?? throw new ArgumentNullException(nameof(transport));
    }

    public HardwareCameraAgentClient(
        string pipeName = HardwareCameraAgentProtocol.DefaultPipeName,
        TimeSpan? connectTimeout = null,
        TimeSpan? responseTimeout = null)
        : this(new NamedPipeHardwareCameraAgentTransport(pipeName, connectTimeout, responseTimeout))
    {
    }

    public async Task<HardwareCameraAgentReply<HardwareSingleReadinessResult>> GetSingleReadinessAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default)
    {
        var request = HardwareCameraAgentProtocolCodec.CreateReadinessRequest(cameraAlias);
        var responseJson = await SendAsync(request, cancellationToken).ConfigureAwait(false);
        return HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            responseJson,
            request.RequestId,
            cameraAlias);
    }

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> CaptureSingleAsync(
        string transactionId,
        string cameraAlias,
        HardwareCaptureSafetyConfirmations confirmations,
        CancellationToken cancellationToken = default) =>
        Task.FromException<HardwareCameraAgentReply<HardwareSingleCaptureResult>>(
            new HardwareProtocolViolationException(
                "CaptureProfileSnapshotRequired",
                "Hardware capture requires the exact approved readiness profile snapshot."));

    public async Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> CaptureSingleAsync(
        string transactionId,
        string cameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        HardwareCaptureSafetyConfirmations confirmations,
        bool liveViewHandoffRequested,
        CancellationToken cancellationToken = default)
    {
        var request = HardwareCameraAgentProtocolCodec.CreateCaptureRequest(
            transactionId,
            cameraAlias,
            expectedProfile,
            confirmations,
            liveViewHandoffRequested);
        var responseJson = await SendAsync(request, cancellationToken).ConfigureAwait(false);
        return HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            responseJson,
            request.RequestId,
            transactionId,
            cameraAlias,
            expectedProfile,
            liveViewHandoffRequested);
    }

    public async Task<HardwareCameraAgentReply<HardwareSingleLiveViewResult>> ProbeLiveViewAsync(
        string cameraAlias,
        HardwareLiveViewSafetyConfirmation confirmation,
        int frames = 1,
        int intervalMs = 100,
        CancellationToken cancellationToken = default)
    {
        var request = HardwareCameraAgentProtocolCodec.CreateLiveViewProbeRequest(
            cameraAlias,
            confirmation,
            frames,
            intervalMs);
        var responseJson = await SendAsync(request, cancellationToken).ConfigureAwait(false);
        return HardwareCameraAgentProtocolCodec.DeserializeLiveViewResponse(
            responseJson,
            request.RequestId,
            cameraAlias,
            frames);
    }

    public async Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        CancellationToken cancellationToken = default)
    {
        var request = HardwareCameraAgentProtocolCodec.CreateTransactionResultRequest(transactionId);
        var responseJson = await SendAsync(request, cancellationToken).ConfigureAwait(false);
        return HardwareCameraAgentProtocolCodec.DeserializeTransactionResultResponse(
            responseJson,
            request.RequestId,
            transactionId);
    }

    public async Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool expectedLiveViewHandoffRequested,
        CancellationToken cancellationToken = default)
    {
        var request = HardwareCameraAgentProtocolCodec.CreateTransactionResultRequest(transactionId);
        var responseJson = await SendAsync(request, cancellationToken).ConfigureAwait(false);
        return HardwareCameraAgentProtocolCodec.DeserializeTransactionResultResponse(
            responseJson,
            request.RequestId,
            transactionId,
            expectedCameraAlias,
            expectedProfile,
            expectedLiveViewHandoffRequested);
    }

    private async Task<string> SendAsync(
        HardwareCameraAgentRequestEnvelope request,
        CancellationToken cancellationToken)
    {
        var requestJson = HardwareCameraAgentProtocolCodec.SerializeRequest(request);
        return await _transport.SendAsync(requestJson, cancellationToken).ConfigureAwait(false);
    }
}
