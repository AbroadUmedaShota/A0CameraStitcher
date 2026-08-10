using System.Security.Cryptography;
using System.Text.Json;

namespace A0CameraStitcher.M3.Foundation.Hardware;

public static class HardwareContinuousLiveViewProtocol
{
    public const string SchemaVersion = "a0.camera-agent.hardware.v2";
    public const string Marker = "Hardware";

    public static class Operations
    {
        public const string Start = "start-live-view";
        public const string ReadFrame = "read-live-view-frame";
        public const string Heartbeat = "live-view-heartbeat";
        public const string Stop = "stop-live-view";
        public const string Close = "close-agent-session";
    }
}

public sealed record HardwareContinuousLiveViewResult
{
    public required string CameraMode { get; init; }
    public required string CameraAlias { get; init; }
    public required string SessionId { get; init; }
    public required string State { get; init; }
    public required ulong FrameNumber { get; init; }
    public required int FrameSize { get; init; }
    public required string FrameSha256 { get; init; }
    public required string FrameJpegBase64 { get; init; }
    public required bool PreviewIsOriginal { get; init; }
    public required bool PreviewIsStitchInput { get; init; }
    public required bool SdkSessionOpen { get; init; }
    public required bool LiveViewRunning { get; init; }
    public required int HeartbeatTimeoutSeconds { get; init; }
    public required int MaximumSessionSeconds { get; init; }
    public required bool RealIdentifiersIncluded { get; init; }
    public required string ErrorCategory { get; init; }
    public required string ErrorDetail { get; init; }

    public byte[] DecodeVerifiedFrame()
    {
        if (State != "Frame" || FrameSize is < 4 or > 512 * 1024 ||
            FrameSha256.Length != 64 || FrameSha256.Any(character =>
                character is not (>= '0' and <= '9') and not (>= 'a' and <= 'f')))
        {
            throw new HardwareProtocolViolationException(
                "InvalidLiveViewFrame", "The continuous Live View frame metadata is invalid.");
        }
        byte[] bytes;
        try
        {
            bytes = Convert.FromBase64String(FrameJpegBase64);
        }
        catch (FormatException exception)
        {
            throw new HardwareProtocolViolationException(
                "InvalidLiveViewFrame", "The continuous Live View frame is not canonical base64.", exception);
        }
        var hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        if (bytes.Length != FrameSize || !CryptographicOperations.FixedTimeEquals(
                Convert.FromHexString(hash), Convert.FromHexString(FrameSha256)) ||
            bytes[0] != 0xFF || bytes[1] != 0xD8 ||
            bytes[^2] != 0xFF || bytes[^1] != 0xD9)
        {
            throw new HardwareProtocolViolationException(
                "InvalidLiveViewFrame", "The continuous Live View JPEG size, hash, or markers are invalid.");
        }
        return bytes;
    }
}

public sealed class HardwareContinuousLiveViewClient
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };
    private readonly IHardwareCameraAgentTransport _transport;

    public HardwareContinuousLiveViewClient(IHardwareCameraAgentTransport transport) =>
        _transport = transport ?? throw new ArgumentNullException(nameof(transport));

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> StartAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        SendAsync(HardwareContinuousLiveViewProtocol.Operations.Start, sessionId, true, cancellationToken);

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> ReadFrameAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        SendAsync(HardwareContinuousLiveViewProtocol.Operations.ReadFrame, sessionId, false, cancellationToken);

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> HeartbeatAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        SendAsync(HardwareContinuousLiveViewProtocol.Operations.Heartbeat, sessionId, false, cancellationToken);

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> StopAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        SendAsync(HardwareContinuousLiveViewProtocol.Operations.Stop, sessionId, false, cancellationToken);

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> CloseAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        SendAsync(HardwareContinuousLiveViewProtocol.Operations.Close, sessionId, false, cancellationToken);

    public static string CreateSessionId() => Guid.NewGuid().ToString("N");

    private async Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> SendAsync(
        string operation,
        string sessionId,
        bool includeStartFields,
        CancellationToken cancellationToken)
    {
        ValidateSessionId(sessionId);
        var requestId = $"request-{Guid.NewGuid():N}";
        object payload = includeStartFields
            ? new { cameraAlias = "CAM-A", sessionId, exclusiveCameraControlConfirmed = true }
            : new { sessionId };
        var request = JsonSerializer.Serialize(new
        {
            schemaVersion = HardwareContinuousLiveViewProtocol.SchemaVersion,
            simulation = false,
            marker = HardwareContinuousLiveViewProtocol.Marker,
            requestId,
            operation,
            payload,
        }, JsonOptions);
        var response = await _transport.SendAsync(request, cancellationToken).ConfigureAwait(false);
        return DeserializeResponse(response, requestId, sessionId, operation);
    }

    internal static HardwareCameraAgentReply<HardwareContinuousLiveViewResult> DeserializeResponse(
        string json,
        string expectedRequestId,
        string expectedSessionId,
        string operation)
    {
        using var document = JsonDocument.Parse(json, new JsonDocumentOptions
        {
            AllowTrailingCommas = false,
            CommentHandling = JsonCommentHandling.Disallow,
        });
        var root = document.RootElement;
        RequireExactProperties(root, "schemaVersion", "simulation", "marker", "requestId", "success", "resultCode", "payload");
        if (root.GetProperty("schemaVersion").GetString() != HardwareContinuousLiveViewProtocol.SchemaVersion ||
            root.GetProperty("simulation").GetBoolean() ||
            root.GetProperty("marker").GetString() != HardwareContinuousLiveViewProtocol.Marker ||
            root.GetProperty("requestId").GetString() != expectedRequestId)
        {
            throw new HardwareProtocolViolationException(
                "InvalidLiveViewEnvelope", "The continuous Live View response envelope is invalid.");
        }
        var success = root.GetProperty("success").GetBoolean();
        var resultCode = root.GetProperty("resultCode").GetString() ?? string.Empty;
        var payloadElement = root.GetProperty("payload");
        if (!payloadElement.TryGetProperty("sessionId", out _))
        {
            var rejection = payloadElement.TryGetProperty("rejectionCode", out var rejectionCode)
                ? rejectionCode.GetString() ?? resultCode : resultCode;
            var detail = payloadElement.TryGetProperty("errorDetail", out var errorDetail)
                ? errorDetail.GetString() ?? string.Empty : string.Empty;
            throw new HardwareCameraAgentRemoteException(
                expectedRequestId, resultCode, rejection, detail);
        }
        RequireExactProperties(
            payloadElement,
            "cameraMode", "cameraAlias", "sessionId", "state", "frameNumber", "frameSize",
            "frameSha256", "frameJpegBase64", "previewIsOriginal", "previewIsStitchInput",
            "sdkSessionOpen", "liveViewRunning", "heartbeatTimeoutSeconds", "maximumSessionSeconds",
            "realIdentifiersIncluded", "errorCategory", "errorDetail");
        var payload = JsonSerializer.Deserialize<HardwareContinuousLiveViewResult>(
            payloadElement.GetRawText(), JsonOptions) ?? throw new HardwareProtocolViolationException(
                "InvalidLiveViewResult", "The continuous Live View payload is missing.");
        ValidatePayload(payload, expectedSessionId, operation, success, resultCode);
        return new HardwareCameraAgentReply<HardwareContinuousLiveViewResult>(
            expectedRequestId, success, resultCode, payload);
    }

    private static void ValidatePayload(
        HardwareContinuousLiveViewResult payload,
        string expectedSessionId,
        string operation,
        bool success,
        string resultCode)
    {
        if (payload.CameraMode != "SingleCamera" || payload.CameraAlias != "CAM-A" ||
            payload.SessionId != expectedSessionId || payload.PreviewIsOriginal ||
            payload.PreviewIsStitchInput || payload.RealIdentifiersIncluded ||
            payload.HeartbeatTimeoutSeconds != 20 || payload.MaximumSessionSeconds != 600)
        {
            throw new HardwareProtocolViolationException(
                "InvalidLiveViewResult", "The continuous Live View result violates product invariants.");
        }
        if (!success)
        {
            if (string.IsNullOrEmpty(payload.ErrorCategory) || resultCode != payload.ErrorCategory ||
                payload.SdkSessionOpen || payload.LiveViewRunning || payload.FrameSize != 0 ||
                payload.FrameJpegBase64.Length != 0)
            {
                throw new HardwareProtocolViolationException(
                    "InvalidLiveViewResult", "The failed continuous Live View result is inconsistent.");
            }
            return;
        }
        var expectedState = operation switch
        {
            HardwareContinuousLiveViewProtocol.Operations.Start => "Started",
            HardwareContinuousLiveViewProtocol.Operations.ReadFrame => "Frame",
            HardwareContinuousLiveViewProtocol.Operations.Heartbeat => "Heartbeat",
            HardwareContinuousLiveViewProtocol.Operations.Stop => "Stopped",
            HardwareContinuousLiveViewProtocol.Operations.Close => "Closed",
            _ => throw new HardwareProtocolViolationException(
                "InvalidLiveViewOperation", "The continuous Live View operation is unsupported."),
        };
        if (resultCode != expectedState || payload.State != expectedState ||
            payload.ErrorCategory.Length != 0 || payload.ErrorDetail.Length != 0)
        {
            throw new HardwareProtocolViolationException(
                "InvalidLiveViewResult", "The continuous Live View success result is inconsistent.");
        }
        var running = expectedState is "Started" or "Frame" or "Heartbeat";
        if (payload.SdkSessionOpen != running || payload.LiveViewRunning != running)
        {
            throw new HardwareProtocolViolationException(
                "InvalidLiveViewResult", "The continuous Live View session flags are inconsistent.");
        }
        if (expectedState == "Frame")
        {
            _ = payload.DecodeVerifiedFrame();
        }
        else if (payload.FrameSize != 0 || payload.FrameSha256.Length != 0 ||
                 payload.FrameJpegBase64.Length != 0)
        {
            throw new HardwareProtocolViolationException(
                "InvalidLiveViewResult", "A non-frame result contains preview bytes.");
        }
    }

    private static void RequireExactProperties(JsonElement element, params string[] expected)
    {
        if (element.ValueKind != JsonValueKind.Object)
        {
            throw new HardwareProtocolViolationException(
                "InvalidLiveViewJson", "A continuous Live View JSON object was required.");
        }
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var property in element.EnumerateObject())
        {
            if (!names.Add(property.Name))
            {
                throw new HardwareProtocolViolationException(
                    "InvalidLiveViewJson", "Duplicate continuous Live View JSON property.");
            }
        }
        if (!names.SetEquals(expected))
        {
            throw new HardwareProtocolViolationException(
                "InvalidLiveViewJson", "Missing or unknown continuous Live View JSON property.");
        }
    }

    private static void ValidateSessionId(string sessionId)
    {
        if (sessionId.Length != 32 || sessionId.Any(character =>
                character is not (>= '0' and <= '9') and not (>= 'a' and <= 'f')))
        {
            throw new ArgumentException("Live View sessionId must be 32 lowercase hexadecimal characters.", nameof(sessionId));
        }
    }
}
