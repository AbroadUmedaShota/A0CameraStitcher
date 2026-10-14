using System.Text;
using System.Text.Json;

namespace A0CameraStitcher.M3.Foundation.Hardware;

/// <summary>
/// Client side of <c>a0.camera-agent.hardware-dual-bound-live-view.v1</c> (Issue #291, ADR-0035).
/// </summary>
/// <remarks>
/// <para>
/// A Live View the operator can open while a binding is Ready, without going back through
/// candidate selection. The schema is the same for every host that serves it; only the envelope
/// <c>marker</c> differs (the binding pipe is <c>HardwareBinding</c>, the capture pipe is
/// <c>Hardware</c>). Only the binding host serves it in this change, so only that host is ever
/// sent a request; the capture host marker exists so the codec does not have to change when the
/// capture host starts serving it.
/// </para>
/// <para>
/// A frame exists only inside the response that carries it. Nothing here logs, caches or writes
/// one, and the decoded bytes are handed to the caller to display and drop.
/// </para>
/// </remarks>
public static class DualBoundLiveViewProtocol
{
    public const string SchemaVersion = "a0.camera-agent.hardware-dual-bound-live-view.v1";

    /// <summary>Raw preview bytes the Agent will return, before base64 expansion.</summary>
    public const int MaximumFrameBytes = 256 * 1024;

    /// <summary>Value of <c>state</c> in a successful <c>start-bound-live-view</c> response.</summary>
    public const string LiveState = "Live";

    public static class Operations
    {
        public const string Start = "start-bound-live-view";
        public const string GetFrame = "get-bound-live-view-frame";
        public const string Stop = "stop-bound-live-view";

        public static IReadOnlyList<string> Required { get; } = Array.AsReadOnly([Start, GetFrame, Stop]);
    }

    public static class ResultCodes
    {
        public const string Started = "BoundLiveViewStarted";
        public const string Frame = "BoundLiveViewFrame";
        public const string Stopped = "BoundLiveViewStopped";
    }

    /// <summary>Refusals this client treats specially. Anything else is a generic refusal.</summary>
    public static class RefusalCodes
    {
        public const string ProtocolRequired = "DualBoundLiveViewProtocolRequired";
        public const string AlreadyOpen = "BoundLiveViewAlreadyOpen";
        public const string NotActive = "BoundLiveViewNotActive";
        public const string Unavailable = "BoundLiveViewUnavailable";
        public const string HostLifetimeInsufficient = "HostLifetimeInsufficient";
        public const string CaptureTransactionActive = "CaptureTransactionActive";
        public const string FrameUnavailable = "LiveViewFrameUnavailable";
        public const string FrameTooLarge = "LiveViewFrameTooLarge";
        public const string BindingNotReady = "BindingNotReady";
        public const string StartFailed = "LiveViewStartFailed";
    }

    public static string MarkerFor(DualBoundLiveViewHost host) => host switch
    {
        DualBoundLiveViewHost.Binding => DualBindingCameraAgentProtocol.Marker,
        DualBoundLiveViewHost.Capture => DualHardwareCameraAgentProtocol.Marker,
        _ => throw new ArgumentOutOfRangeException(nameof(host)),
    };
}

/// <summary>Which host's pipe a request is addressed to. Decides the envelope marker.</summary>
public enum DualBoundLiveViewHost
{
    Binding,
    Capture,
}

public sealed record DualBoundLiveViewStartResult
{
    public required string SessionId { get; init; }
    public required string CameraAlias { get; init; }

    /// <summary>
    /// Milliseconds the host will still admit a capture for, as the Agent computed them on its own
    /// clock when it answered. Null when the Agent did not report it.
    /// </summary>
    public long? CaptureAdmissionRemainingMs { get; init; }
}

public sealed record DualBoundLiveViewFrameResult
{
    public required string SessionId { get; init; }
    public required string CameraAlias { get; init; }
    public required int FrameBytes { get; init; }

    /// <summary>Transient preview. Held only long enough to display it; never written anywhere.</summary>
    public required byte[] Frame { get; init; }

    public long? CaptureAdmissionRemainingMs { get; init; }
}

public sealed record DualBoundLiveViewStopResult
{
    public required string SessionId { get; init; }
    public required bool LiveViewStopped { get; init; }
    public required bool SdkSourceClosed { get; init; }

    /// <summary>False when nothing was open (stop is idempotent).</summary>
    public required bool WasOpen { get; init; }

    public long? CaptureAdmissionRemainingMs { get; init; }

    /// <summary>Both the Live View and the SDK source were confirmed closed by the Agent.</summary>
    public bool ConfirmedClosed => LiveViewStopped && SdkSourceClosed;
}

public static class DualBoundLiveViewProtocolCodec
{
    private const int MaximumRequestJsonBytes = 64 * 1024;

    // Same asymmetry as the binding schema: a frame response needs the full pipe frame.
    private const int MaximumResponseJsonBytes = 1024 * 1024;

    public static string CreateStartRequest(
        string requestId,
        DualBoundLiveViewHost host,
        string sessionId,
        string cameraAlias) =>
        Serialize(
            requestId,
            host,
            DualBoundLiveViewProtocol.Operations.Start,
            writer => WriteSessionAndAlias(writer, sessionId, cameraAlias));

    public static string CreateFrameRequest(
        string requestId,
        DualBoundLiveViewHost host,
        string sessionId,
        string cameraAlias) =>
        Serialize(
            requestId,
            host,
            DualBoundLiveViewProtocol.Operations.GetFrame,
            writer => WriteSessionAndAlias(writer, sessionId, cameraAlias));

    public static string CreateStopRequest(string requestId, DualBoundLiveViewHost host, string sessionId) =>
        Serialize(
            requestId,
            host,
            DualBoundLiveViewProtocol.Operations.Stop,
            writer => writer.WriteString("sessionId", DualBindingCameraAgentProtocolCodec.ValidatedSessionId(sessionId)));

    /// <summary>
    /// Local fail-closed response used when the process generation that owns the binding disappears
    /// before or during dispatch. Same shape as a native refusal, so every operation clears the
    /// stale session through its normal response path.
    /// </summary>
    public static string CreateBindingHostExpiredResponse(
        string requestId,
        DualBoundLiveViewHost host,
        string sessionId)
    {
        DualBindingCameraAgentProtocolCodec.ValidateRequestId(requestId);
        var validatedSessionId = DualBindingCameraAgentProtocolCodec.ValidatedSessionId(sessionId);
        using var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject();
            writer.WriteString("schemaVersion", DualBoundLiveViewProtocol.SchemaVersion);
            writer.WriteBoolean("simulation", false);
            writer.WriteString("marker", DualBoundLiveViewProtocol.MarkerFor(host));
            writer.WriteString("requestId", requestId);
            writer.WriteBoolean("success", false);
            writer.WriteString("resultCode", "BindingHostExpired");
            writer.WritePropertyName("payload");
            writer.WriteStartObject();
            writer.WriteString("sessionId", validatedSessionId);
            writer.WriteString("state", DualBindingSessionState.Invalid.ToString());
            writer.WriteString("invalidationReason", DualBindingInvalidationReason.AgentRestart.ToString());
            writer.WriteString(
                "detail",
                "The Camera Agent process that created this binding expired; no replacement Agent was used.");
            writer.WriteEndObject();
            writer.WriteEndObject();
        }

        return Encoding.UTF8.GetString(buffer.ToArray());
    }

    public static DualBindingReply<DualBoundLiveViewStartResult> DeserializeStartResponse(
        string json,
        string expectedRequestId,
        DualBoundLiveViewHost host,
        string expectedSessionId,
        string expectedAlias)
    {
        using var document = ParseResponse(json, expectedRequestId, host, out var success, out var resultCode);
        var payload = document.RootElement.GetProperty("payload");
        if (!success)
        {
            return DualBindingReply<DualBoundLiveViewStartResult>.Refused(
                DualBindingCameraAgentProtocolCodec.ReadRefusal(resultCode, payload));
        }

        if (resultCode != DualBoundLiveViewProtocol.ResultCodes.Started ||
            ReadString(payload, "state") != DualBoundLiveViewProtocol.LiveState)
        {
            throw Violation("ForgedBindingResponse", "A successful start response did not confirm a Live state.");
        }

        return DualBindingReply<DualBoundLiveViewStartResult>.Accepted(new DualBoundLiveViewStartResult
        {
            SessionId = RequireSessionMatch(payload, expectedSessionId),
            CameraAlias = RequireAliasMatch(payload, expectedAlias),
            CaptureAdmissionRemainingMs = ReadOptionalRemainingMs(payload),
        });
    }

    public static DualBindingReply<DualBoundLiveViewFrameResult> DeserializeFrameResponse(
        string json,
        string expectedRequestId,
        DualBoundLiveViewHost host,
        string expectedSessionId,
        string expectedAlias)
    {
        using var document = ParseResponse(json, expectedRequestId, host, out var success, out var resultCode);
        var payload = document.RootElement.GetProperty("payload");
        if (!success)
        {
            return DualBindingReply<DualBoundLiveViewFrameResult>.Refused(
                DualBindingCameraAgentProtocolCodec.ReadRefusal(resultCode, payload));
        }

        if (resultCode != DualBoundLiveViewProtocol.ResultCodes.Frame)
        {
            throw Violation("ForgedBindingResponse", "A successful frame response has the wrong result code.");
        }

        var declaredBytes = ReadInt32(payload, "frameBytes");
        byte[] frame;
        try
        {
            frame = Convert.FromBase64String(ReadString(payload, "frameBase64"));
        }
        catch (FormatException exception)
        {
            throw Violation("InvalidPayload", "The Live View preview is not valid base64.", exception);
        }

        // The declared count and the decoded length must agree, and an empty or oversized preview is
        // never a preview the operator should decide from. (The Agent reports those as refusals.)
        if (frame.Length != declaredBytes || frame.Length is 0 or > DualBoundLiveViewProtocol.MaximumFrameBytes)
        {
            throw Violation("InvalidPayload", "The Live View preview length does not match the declared size.");
        }

        return DualBindingReply<DualBoundLiveViewFrameResult>.Accepted(new DualBoundLiveViewFrameResult
        {
            SessionId = RequireSessionMatch(payload, expectedSessionId),
            CameraAlias = RequireAliasMatch(payload, expectedAlias),
            FrameBytes = declaredBytes,
            Frame = frame,
            CaptureAdmissionRemainingMs = ReadOptionalRemainingMs(payload),
        });
    }

    public static DualBindingReply<DualBoundLiveViewStopResult> DeserializeStopResponse(
        string json,
        string expectedRequestId,
        DualBoundLiveViewHost host,
        string expectedSessionId)
    {
        using var document = ParseResponse(json, expectedRequestId, host, out var success, out var resultCode);
        var payload = document.RootElement.GetProperty("payload");
        if (!success)
        {
            return DualBindingReply<DualBoundLiveViewStopResult>.Refused(
                DualBindingCameraAgentProtocolCodec.ReadRefusal(resultCode, payload));
        }

        if (resultCode != DualBoundLiveViewProtocol.ResultCodes.Stopped)
        {
            throw Violation("ForgedBindingResponse", "A successful stop response has the wrong result code.");
        }

        return DualBindingReply<DualBoundLiveViewStopResult>.Accepted(new DualBoundLiveViewStopResult
        {
            SessionId = RequireSessionMatch(payload, expectedSessionId),
            LiveViewStopped = ReadBoolean(payload, "liveViewStopped"),
            SdkSourceClosed = ReadBoolean(payload, "sdkSourceClosed"),
            WasOpen = ReadBoolean(payload, "wasOpen"),
            CaptureAdmissionRemainingMs = ReadOptionalRemainingMs(payload),
        });
    }

    private static string Serialize(
        string requestId,
        DualBoundLiveViewHost host,
        string operation,
        Action<Utf8JsonWriter> writePayload)
    {
        DualBindingCameraAgentProtocolCodec.ValidateRequestId(requestId);
        if (!DualBoundLiveViewProtocol.Operations.Required.Contains(operation, StringComparer.Ordinal))
        {
            throw Violation("UnsupportedOperation", "The bound Live View operation is unsupported.");
        }

        using var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject();
            writer.WriteString("schemaVersion", DualBoundLiveViewProtocol.SchemaVersion);
            writer.WriteBoolean("simulation", false);
            writer.WriteString("marker", DualBoundLiveViewProtocol.MarkerFor(host));
            writer.WriteString("requestId", requestId);
            writer.WriteString("operation", operation);
            writer.WritePropertyName("payload");
            writer.WriteStartObject();
            writePayload(writer);
            writer.WriteEndObject();
            writer.WriteEndObject();
        }

        var json = Encoding.UTF8.GetString(buffer.ToArray());
        if (Encoding.UTF8.GetByteCount(json) > MaximumRequestJsonBytes)
        {
            throw Violation("MalformedEnvelope", "The bound Live View request is too large.");
        }

        return json;
    }

    private static void WriteSessionAndAlias(Utf8JsonWriter writer, string sessionId, string cameraAlias)
    {
        if (!DualBindingCameraAgentProtocol.OrderedAliases.Contains(cameraAlias, StringComparer.Ordinal))
        {
            // Refused before it reaches the wire: the rig defines only CAM-A and CAM-B.
            throw Violation("UnknownCameraAlias", "The rig defines only CAM-A and CAM-B.");
        }

        writer.WriteString("sessionId", DualBindingCameraAgentProtocolCodec.ValidatedSessionId(sessionId));
        writer.WriteString("cameraAlias", cameraAlias);
    }

    private static JsonDocument ParseResponse(
        string json,
        string expectedRequestId,
        DualBoundLiveViewHost host,
        out bool success,
        out string resultCode)
    {
        if (string.IsNullOrEmpty(json) || Encoding.UTF8.GetByteCount(json) > MaximumResponseJsonBytes)
        {
            throw Violation("MalformedEnvelope", "The bound Live View JSON size is invalid.");
        }

        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(json, new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 32,
            });
        }
        catch (JsonException exception)
        {
            throw Violation("MalformedEnvelope", "The bound Live View response is malformed.", exception);
        }

        try
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                throw Violation("MalformedEnvelope", "The bound Live View response is not an object.");
            }

            if (ReadString(root, "schemaVersion") != DualBoundLiveViewProtocol.SchemaVersion ||
                ReadString(root, "marker") != DualBoundLiveViewProtocol.MarkerFor(host) ||
                ReadBoolean(root, "simulation"))
            {
                // A binding or Dual v2 response reaching this parser, or one from the other host, is
                // a wiring mistake -- not something to interpret.
                throw Violation(
                    DualBoundLiveViewProtocol.RefusalCodes.ProtocolRequired,
                    "The response is not the required bound Live View v1 hardware protocol.");
            }

            if (!string.Equals(ReadString(root, "requestId"), expectedRequestId, StringComparison.Ordinal))
            {
                throw Violation("RequestIdMismatch", "The bound Live View response does not match the request.");
            }

            success = ReadBoolean(root, "success");
            resultCode = ReadString(root, "resultCode");
            if (!root.TryGetProperty("payload", out var payload) || payload.ValueKind != JsonValueKind.Object)
            {
                throw Violation("InvalidPayload", "The bound Live View payload must be an object.");
            }

            return document;
        }
        catch
        {
            document.Dispose();
            throw;
        }
    }

    private static string RequireSessionMatch(JsonElement payload, string expectedSessionId)
    {
        var sessionId = DualBindingCameraAgentProtocolCodec.ValidatedSessionId(ReadString(payload, "sessionId"));
        if (!string.Equals(sessionId, expectedSessionId, StringComparison.Ordinal))
        {
            throw Violation("SessionIdMismatch", "The response names a different binding session than the request.");
        }

        return sessionId;
    }

    private static string RequireAliasMatch(JsonElement payload, string expectedAlias)
    {
        var alias = ReadString(payload, "cameraAlias");
        if (!string.Equals(alias, expectedAlias, StringComparison.Ordinal))
        {
            // A Live View that names a different camera than the one asked for is the failure the
            // whole binding design exists to prevent, so it never becomes a displayed success.
            throw Violation("ForgedBindingResponse", "The Live View camera is not the camera that was requested.");
        }

        return alias;
    }

    private static long? ReadOptionalRemainingMs(JsonElement payload)
    {
        if (!payload.TryGetProperty("captureAdmissionRemainingMs", out var value))
        {
            return null;
        }

        if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt64(out var milliseconds) || milliseconds < 0)
        {
            throw Violation("InvalidPayload", "captureAdmissionRemainingMs is not a non-negative integer.");
        }

        return milliseconds;
    }

    private static string ReadString(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : throw Violation("InvalidPayload", $"The bound Live View field '{name}' is missing or not a string.");

    private static bool ReadBoolean(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? value.GetBoolean()
            : throw Violation("InvalidPayload", $"The bound Live View field '{name}' is missing or not a boolean.");

    private static int ReadInt32(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt32(out var parsed)
            ? parsed
            : throw Violation("InvalidPayload", $"The bound Live View field '{name}' is missing or not an integer.");

    private static HardwareProtocolViolationException Violation(
        string errorCode,
        string message,
        Exception? innerException = null) =>
        DualBindingCameraAgentProtocolCodec.Violation(errorCode, message, innerException);
}
