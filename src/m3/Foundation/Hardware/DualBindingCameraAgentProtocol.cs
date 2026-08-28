using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace A0CameraStitcher.M3.Foundation.Hardware;

/// <summary>
/// Client side of <c>a0.camera-agent.hardware-dual-binding.v1</c> (ADR-0025, Issue #61/#62).
/// </summary>
/// <remarks>
/// The transport is deliberately the existing <see cref="IHardwareCameraAgentTransport"/>: the
/// binding host shares the same pipe loop as the Single and Dual v2 hosts, so it has the same
/// framing, the same 1 MiB bound and the same delivery acknowledgment. A second transport would
/// only create a second place for those to drift.
/// </remarks>
public static class DualBindingCameraAgentProtocol
{
    public const string SchemaVersion = "a0.camera-agent.hardware-dual-binding.v1";

    /// <summary>
    /// Deliberately not the Dual v2 <c>Hardware</c> marker. The two protocols are mutually
    /// unreadable, so neither dispatcher can accept the other's request by accident.
    /// </summary>
    public const string Marker = "HardwareBinding";

    public const string DefaultPipeName = "A0CameraStitcher.CameraAgent.HardwareDualBinding.v1";

    public const string CameraMode = "DualCamera";
    public const string CameraAliasA = "CAM-A";
    public const string CameraAliasB = "CAM-B";

    /// <summary>
    /// Number of candidates a binding session accepts. Not "at least two": three connected bodies
    /// means the operator cannot know which two the app picked, so that is a rejection.
    /// </summary>
    public const int RequiredCandidateCount = 2;

    /// <summary>Raw preview bytes the agent will return, before base64 expansion.</summary>
    public const int MaximumLiveViewFrameBytes = 256 * 1024;

    public static class Operations
    {
        public const string BeginBinding = "begin-binding";
        public const string StartCandidateLiveView = "start-candidate-live-view";
        public const string GetCandidateLiveViewFrame = "get-candidate-live-view-frame";
        public const string ConfirmAlias = "confirm-alias";
        public const string CompleteBinding = "complete-binding";
        public const string ActivateCapture = "activate-capture";
        public const string CancelBinding = "cancel-binding";

        public static IReadOnlyList<string> Required { get; } = Array.AsReadOnly([
            BeginBinding,
            StartCandidateLiveView,
            GetCandidateLiveViewFrame,
            ConfirmAlias,
            CompleteBinding,
            ActivateCapture,
            CancelBinding,
        ]);
    }

    public static IReadOnlyList<string> OrderedAliases { get; } =
        Array.AsReadOnly([CameraAliasA, CameraAliasB]);
}

/// <summary>Why a Ready binding stopped being trustworthy. Mirrors the native reasons exactly.</summary>
public enum DualBindingInvalidationReason
{
    None,
    AgentRestart,
    UsbReconnect,
    CameraCountChanged,
    TopologyChanged,
    SdkManagerRecreated,
    SdkError,
}

public enum DualBindingSessionState
{
    None,
    CollectingCandidates,
    AwaitingQuiesce,
    Ready,
    Invalid,
}

/// <summary>
/// Why the agent refused an operation. This is a value, not an exception: every one of these is
/// something the operator has to be shown and can act on, unlike a malformed envelope.
/// </summary>
public sealed record DualBindingRefusal
{
    public required string ResultCode { get; init; }
    public string SessionId { get; init; } = string.Empty;
    public DualBindingSessionState State { get; init; } = DualBindingSessionState.None;
    public DualBindingInvalidationReason InvalidationReason { get; init; } = DualBindingInvalidationReason.None;
    public string Detail { get; init; } = string.Empty;

    /// <summary>The session is gone or untrustworthy; only a fresh binding can recover.</summary>
    public bool RequiresRebinding =>
        ResultCode is "BindingInvalidated" or "SessionMismatch" or "CandidateNotQuiesced" or
            "BindingCleanupFailed";
}

/// <summary>Success value or a typed refusal. Exactly one is present.</summary>
public sealed record DualBindingReply<T> where T : class
{
    private DualBindingReply(T? value, DualBindingRefusal? refusal)
    {
        Value = value;
        Refusal = refusal;
    }

    public T? Value { get; }

    public DualBindingRefusal? Refusal { get; }

    public bool Succeeded => Value is not null;

    public static DualBindingReply<T> Accepted(T value) => new(value, null);

    public static DualBindingReply<T> Refused(DualBindingRefusal refusal) => new(null, refusal);
}

public sealed record DualBindingBeginResult
{
    public required string SessionId { get; init; }
    public required DualBindingSessionState State { get; init; }
    public required IReadOnlyList<int> CandidateOrdinals { get; init; }
}

public sealed record DualBindingLiveViewResult
{
    public required string SessionId { get; init; }
    public required int CandidateOrdinal { get; init; }
    public required bool LiveViewActive { get; init; }
}

public sealed record DualBindingFrameResult
{
    public required string SessionId { get; init; }
    public required int CandidateOrdinal { get; init; }
    public required int FrameBytes { get; init; }

    /// <summary>
    /// Transient preview. Held only long enough to display it; never written anywhere.
    /// </summary>
    public required byte[] Frame { get; init; }
}

public sealed record DualBindingAliasResult
{
    public required string SessionId { get; init; }
    public required int CandidateOrdinal { get; init; }
    public required string CameraAlias { get; init; }
    public required DualBindingSessionState State { get; init; }
}

/// <summary>
/// Exactly the ADR-0025 allowlist. Adding a field here is a decision change, not an implementation
/// detail: previews, raw identifiers, serials, source objects and candidate ordinals are excluded.
/// </summary>
public sealed record DualBindingEvidence
{
    public required string CameraAlias { get; init; }
    public required string ProviderId { get; init; }
    public required int ProviderVersion { get; init; }
    public required string ConfirmedAtUtc { get; init; }
    public required string InvalidationReason { get; init; }
}

public sealed record DualBindingCompleteResult
{
    public required string SessionId { get; init; }
    public required DualBindingSessionState State { get; init; }
    public required IReadOnlyList<DualBindingEvidence> Evidence { get; init; }
}

public sealed record DualBindingCaptureActivationResult
{
    public required string SessionId { get; init; }
    public required DualBindingSessionState State { get; init; }
    public required bool CaptureHostActivated { get; init; }
}

public sealed record DualBindingCancellationResult
{
    public required string SessionId { get; init; }
    public required DualBindingSessionState State { get; init; }
    public required bool LiveViewStopped { get; init; }
    public required bool SdkSessionClosed { get; init; }
    public required bool SdkSessionEnded { get; init; }
}

public static class DualBindingCameraAgentProtocolCodec
{
    /// <summary>
    /// Requests are small by construction -- an operation name, a session id, an ordinal and an
    /// alias -- so they get the same tight bound the other protocols use.
    /// </summary>
    private const int MaximumRequestJsonBytes = 256 * 1024;

    /// <summary>
    /// Responses need the full pipe frame, and this asymmetry is deliberate.
    /// <c>get-candidate-live-view-frame</c> carries a preview of up to
    /// <see cref="DualBindingCameraAgentProtocol.MaximumLiveViewFrameBytes"/> raw bytes, which is
    /// about 342 KB once base64 expands it -- comfortably over the request bound. Reusing the
    /// request bound here would reject perfectly valid frames, and it would do it only for large
    /// previews, which is exactly the case nobody tests by hand.
    /// </summary>
    private const int MaximumResponseJsonBytes = 1024 * 1024;

    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        RespectRequiredConstructorParameters = true,
        WriteIndented = false,
        MaxDepth = 32,
        Converters = { new JsonStringEnumConverter() },
    };

    public static string CreateBeginBindingRequest(string requestId) =>
        Serialize(
            requestId,
            DualBindingCameraAgentProtocol.Operations.BeginBinding,
            writer =>
            {
                writer.WriteString("cameraMode", DualBindingCameraAgentProtocol.CameraMode);
            });

    public static string CreateStartLiveViewRequest(string requestId, string sessionId, int candidateOrdinal) =>
        Serialize(
            requestId,
            DualBindingCameraAgentProtocol.Operations.StartCandidateLiveView,
            writer => WriteSessionAndOrdinal(writer, sessionId, candidateOrdinal));

    public static string CreateFrameRequest(string requestId, string sessionId, int candidateOrdinal) =>
        Serialize(
            requestId,
            DualBindingCameraAgentProtocol.Operations.GetCandidateLiveViewFrame,
            writer => WriteSessionAndOrdinal(writer, sessionId, candidateOrdinal));

    public static string CreateConfirmAliasRequest(
        string requestId,
        string sessionId,
        int candidateOrdinal,
        string cameraAlias)
    {
        if (!DualBindingCameraAgentProtocol.OrderedAliases.Contains(cameraAlias, StringComparer.Ordinal))
        {
            // Refused before it reaches the wire. The agent would refuse it too, but sending a
            // known-bad alias would burn the operator's one chance to assign this candidate.
            throw Violation("UnknownCameraAlias", "The rig defines only CAM-A and CAM-B.");
        }

        return Serialize(
            requestId,
            DualBindingCameraAgentProtocol.Operations.ConfirmAlias,
            writer =>
            {
                WriteSessionAndOrdinal(writer, sessionId, candidateOrdinal);
                writer.WriteString("cameraAlias", cameraAlias);
            });
    }

    public static string CreateCompleteBindingRequest(
        string requestId,
        string sessionId,
        DateTimeOffset confirmedAtUtc)
    {
        if (confirmedAtUtc.Offset != TimeSpan.Zero)
        {
            throw Violation("ConfirmedAtMissing", "The binding confirmation time must be UTC.");
        }

        return Serialize(
            requestId,
            DualBindingCameraAgentProtocol.Operations.CompleteBinding,
            writer =>
            {
                writer.WriteString("sessionId", ValidatedSessionId(sessionId));
                writer.WriteString("confirmedAtUtc", confirmedAtUtc.ToString("yyyy-MM-ddTHH:mm:ssZ", CultureInfo.InvariantCulture));
            });
    }

    public static string CreateActivateCaptureRequest(string requestId, string sessionId) =>
        Serialize(
            requestId,
            DualBindingCameraAgentProtocol.Operations.ActivateCapture,
            writer => writer.WriteString("sessionId", ValidatedSessionId(sessionId)));

    public static string CreateCancelBindingRequest(string requestId, string sessionId) =>
        Serialize(
            requestId,
            DualBindingCameraAgentProtocol.Operations.CancelBinding,
            writer => writer.WriteString("sessionId", ValidatedSessionId(sessionId)));

    public static DualBindingReply<DualBindingBeginResult> DeserializeBeginBindingResponse(
        string json,
        string expectedRequestId)
    {
        using var document = ParseResponse(json, expectedRequestId, out var success, out var resultCode);
        var payload = document.RootElement.GetProperty("payload");
        if (!success)
        {
            return DualBindingReply<DualBindingBeginResult>.Refused(ReadRefusal(resultCode, payload));
        }

        if (resultCode != "BindingSessionStarted")
        {
            throw Violation("ForgedBindingResponse", "A successful begin-binding response has the wrong result code.");
        }

        var ordinals = ReadOrdinals(payload);
        if (ordinals.Count != DualBindingCameraAgentProtocol.RequiredCandidateCount)
        {
            // The agent is supposed to have refused any other count. If a "success" arrives with a
            // different one, the operator would be shown a chooser that cannot produce a valid
            // binding, so this is a protocol violation rather than something to display.
            throw Violation(
                "ForgedBindingResponse",
                "A started binding session must offer exactly two candidates.");
        }

        return DualBindingReply<DualBindingBeginResult>.Accepted(new DualBindingBeginResult
        {
            SessionId = ReadSessionId(payload),
            State = ReadState(payload),
            CandidateOrdinals = ordinals,
        });
    }

    public static DualBindingReply<DualBindingLiveViewResult> DeserializeStartLiveViewResponse(
        string json,
        string expectedRequestId,
        string expectedSessionId)
    {
        using var document = ParseResponse(json, expectedRequestId, out var success, out var resultCode);
        var payload = document.RootElement.GetProperty("payload");
        if (!success)
        {
            return DualBindingReply<DualBindingLiveViewResult>.Refused(ReadRefusal(resultCode, payload));
        }

        if (resultCode != "CandidateLiveViewStarted" || !ReadBoolean(payload, "liveViewActive"))
        {
            throw Violation("ForgedBindingResponse", "A successful Live View response did not confirm an active Live View.");
        }

        return DualBindingReply<DualBindingLiveViewResult>.Accepted(new DualBindingLiveViewResult
        {
            SessionId = RequireSessionMatch(payload, expectedSessionId),
            CandidateOrdinal = ReadOrdinal(payload),
            LiveViewActive = true,
        });
    }

    public static DualBindingReply<DualBindingFrameResult> DeserializeFrameResponse(
        string json,
        string expectedRequestId,
        string expectedSessionId)
    {
        using var document = ParseResponse(json, expectedRequestId, out var success, out var resultCode);
        var payload = document.RootElement.GetProperty("payload");
        if (!success)
        {
            return DualBindingReply<DualBindingFrameResult>.Refused(ReadRefusal(resultCode, payload));
        }

        if (resultCode != "CandidateLiveViewFrame")
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

        // The declared count and the decoded length must agree. A preview that is not the length
        // the agent said it was is not a preview the operator should decide from.
        if (frame.Length != declaredBytes ||
            frame.Length is 0 or > DualBindingCameraAgentProtocol.MaximumLiveViewFrameBytes)
        {
            throw Violation("InvalidPayload", "The Live View preview length does not match the declared size.");
        }

        return DualBindingReply<DualBindingFrameResult>.Accepted(new DualBindingFrameResult
        {
            SessionId = RequireSessionMatch(payload, expectedSessionId),
            CandidateOrdinal = ReadOrdinal(payload),
            FrameBytes = declaredBytes,
            Frame = frame,
        });
    }

    public static DualBindingReply<DualBindingAliasResult> DeserializeConfirmAliasResponse(
        string json,
        string expectedRequestId,
        string expectedSessionId,
        string expectedAlias)
    {
        using var document = ParseResponse(json, expectedRequestId, out var success, out var resultCode);
        var payload = document.RootElement.GetProperty("payload");
        if (!success)
        {
            return DualBindingReply<DualBindingAliasResult>.Refused(ReadRefusal(resultCode, payload));
        }

        if (resultCode != "CandidateAliasConfirmed")
        {
            throw Violation("ForgedBindingResponse", "A successful alias confirmation has the wrong result code.");
        }

        var alias = ReadString(payload, "cameraAlias");
        if (!string.Equals(alias, expectedAlias, StringComparison.Ordinal))
        {
            // A confirmation that names a different alias than the operator chose is the one
            // failure this whole design exists to prevent, so it never becomes a displayed success.
            throw Violation("ForgedBindingResponse", "The confirmed alias is not the alias that was requested.");
        }

        return DualBindingReply<DualBindingAliasResult>.Accepted(new DualBindingAliasResult
        {
            SessionId = RequireSessionMatch(payload, expectedSessionId),
            CandidateOrdinal = ReadOrdinal(payload),
            CameraAlias = alias,
            State = ReadState(payload),
        });
    }

    public static DualBindingReply<DualBindingCompleteResult> DeserializeCompleteBindingResponse(
        string json,
        string expectedRequestId,
        string expectedSessionId)
    {
        using var document = ParseResponse(json, expectedRequestId, out var success, out var resultCode);
        var payload = document.RootElement.GetProperty("payload");
        if (!success)
        {
            return DualBindingReply<DualBindingCompleteResult>.Refused(ReadRefusal(resultCode, payload));
        }

        if (resultCode != "BindingCompleted" || ReadState(payload) != DualBindingSessionState.Ready)
        {
            throw Violation("ForgedBindingResponse", "A completed binding response did not report Ready.");
        }

        if (!payload.TryGetProperty("evidence", out var evidenceElement) ||
            evidenceElement.ValueKind != JsonValueKind.Array)
        {
            throw Violation("InvalidPayload", "A completed binding response carries no evidence array.");
        }

        var evidence = new List<DualBindingEvidence>();
        foreach (var entry in evidenceElement.EnumerateArray())
        {
            evidence.Add(
                entry.Deserialize<DualBindingEvidence>(SerializerOptions)
                ?? throw Violation("InvalidPayload", "A binding evidence entry is empty."));
        }

        var aliases = evidence.Select(item => item.CameraAlias).ToList();
        if (!aliases.OrderBy(alias => alias, StringComparer.Ordinal)
            .SequenceEqual(DualBindingCameraAgentProtocol.OrderedAliases, StringComparer.Ordinal))
        {
            throw Violation(
                "ForgedBindingResponse",
                "A completed binding must publish exactly one evidence record for CAM-A and CAM-B.");
        }

        return DualBindingReply<DualBindingCompleteResult>.Accepted(new DualBindingCompleteResult
        {
            SessionId = RequireSessionMatch(payload, expectedSessionId),
            State = DualBindingSessionState.Ready,
            Evidence = evidence.AsReadOnly(),
        });
    }

    public static DualBindingReply<DualBindingCaptureActivationResult> DeserializeActivateCaptureResponse(
        string json,
        string expectedRequestId,
        string expectedSessionId)
    {
        using var document = ParseResponse(json, expectedRequestId, out var success, out var resultCode);
        var payload = document.RootElement.GetProperty("payload");
        if (!success)
        {
            return DualBindingReply<DualBindingCaptureActivationResult>.Refused(ReadRefusal(resultCode, payload));
        }

        if (resultCode != "CaptureHostActivated" ||
            ReadState(payload) != DualBindingSessionState.Ready ||
            !ReadBoolean(payload, "captureHostActivated"))
        {
            throw Violation("ForgedBindingResponse", "Capture activation did not preserve a Ready binding.");
        }

        return DualBindingReply<DualBindingCaptureActivationResult>.Accepted(
            new DualBindingCaptureActivationResult
            {
                SessionId = RequireSessionMatch(payload, expectedSessionId),
                State = DualBindingSessionState.Ready,
                CaptureHostActivated = true,
            });
    }

    public static DualBindingReply<DualBindingCancellationResult> DeserializeCancelBindingResponse(
        string json,
        string expectedRequestId,
        string expectedSessionId)
    {
        using var document = ParseResponse(json, expectedRequestId, out var success, out var resultCode);
        var payload = document.RootElement.GetProperty("payload");
        if (!success)
        {
            return DualBindingReply<DualBindingCancellationResult>.Refused(ReadRefusal(resultCode, payload));
        }

        if (resultCode != "BindingCancelled" ||
            ReadState(payload) != DualBindingSessionState.None ||
            !ReadBoolean(payload, "liveViewStopped") ||
            !ReadBoolean(payload, "sdkSessionClosed") ||
            !ReadBoolean(payload, "sdkSessionEnded"))
        {
            throw Violation("ForgedBindingResponse", "Binding cancellation did not confirm complete SDK cleanup.");
        }

        return DualBindingReply<DualBindingCancellationResult>.Accepted(
            new DualBindingCancellationResult
            {
                SessionId = RequireSessionMatch(payload, expectedSessionId),
                State = DualBindingSessionState.None,
                LiveViewStopped = true,
                SdkSessionClosed = true,
                SdkSessionEnded = true,
            });
    }

    private static string Serialize(string requestId, string operation, Action<Utf8JsonWriter> writePayload)
    {
        ValidateRequestId(requestId);
        if (!DualBindingCameraAgentProtocol.Operations.Required.Contains(operation, StringComparer.Ordinal))
        {
            throw Violation("UnsupportedOperation", "The Dual binding Agent operation is unsupported.");
        }

        using var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer))
        {
            writer.WriteStartObject();
            writer.WriteString("schemaVersion", DualBindingCameraAgentProtocol.SchemaVersion);
            writer.WriteBoolean("simulation", false);
            writer.WriteString("marker", DualBindingCameraAgentProtocol.Marker);
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
            throw Violation("MalformedEnvelope", "The Dual binding Agent request is too large.");
        }

        return json;
    }

    private static void WriteSessionAndOrdinal(Utf8JsonWriter writer, string sessionId, int candidateOrdinal)
    {
        writer.WriteString("sessionId", ValidatedSessionId(sessionId));
        writer.WriteNumber("candidateOrdinal", ValidatedOrdinal(candidateOrdinal));
    }

    private static JsonDocument ParseResponse(
        string json,
        string expectedRequestId,
        out bool success,
        out string resultCode)
    {
        if (string.IsNullOrEmpty(json) || Encoding.UTF8.GetByteCount(json) > MaximumResponseJsonBytes)
        {
            throw Violation("MalformedEnvelope", "The Dual binding Agent JSON size is invalid.");
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
            throw Violation("MalformedEnvelope", "The Dual binding Agent response is malformed.", exception);
        }

        try
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                throw Violation("MalformedEnvelope", "The Dual binding Agent response is not an object.");
            }

            if (ReadString(root, "schemaVersion") != DualBindingCameraAgentProtocol.SchemaVersion ||
                ReadString(root, "marker") != DualBindingCameraAgentProtocol.Marker ||
                ReadBoolean(root, "simulation"))
            {
                // A Dual v2 response reaching this parser, or a simulated one reaching the hardware
                // client, is a wiring mistake -- not something to interpret.
                throw Violation(
                    "DualBindingProtocolRequired",
                    "The response is not the required Dual binding v1 hardware protocol.");
            }

            if (!string.Equals(ReadString(root, "requestId"), expectedRequestId, StringComparison.Ordinal))
            {
                throw Violation("RequestIdMismatch", "The Dual binding Agent response does not match the request.");
            }

            success = ReadBoolean(root, "success");
            resultCode = ReadString(root, "resultCode");
            if (!root.TryGetProperty("payload", out var payload) || payload.ValueKind != JsonValueKind.Object)
            {
                throw Violation("InvalidPayload", "The Dual binding Agent payload must be an object.");
            }

            return document;
        }
        catch
        {
            document.Dispose();
            throw;
        }
    }

    private static DualBindingRefusal ReadRefusal(string resultCode, JsonElement payload) => new()
    {
        ResultCode = resultCode,
        SessionId = payload.TryGetProperty("sessionId", out var sessionId) && sessionId.ValueKind == JsonValueKind.String
            ? sessionId.GetString() ?? string.Empty
            : string.Empty,
        State = payload.TryGetProperty("state", out _)
            ? ReadState(payload)
            : DualBindingSessionState.None,
        InvalidationReason = payload.TryGetProperty("invalidationReason", out var reason) &&
            reason.ValueKind == JsonValueKind.String &&
            Enum.TryParse<DualBindingInvalidationReason>(reason.GetString(), out var parsed)
                ? parsed
                : DualBindingInvalidationReason.None,
        Detail = payload.TryGetProperty("detail", out var detail) && detail.ValueKind == JsonValueKind.String
            ? detail.GetString() ?? string.Empty
            : string.Empty,
    };

    private static IReadOnlyList<int> ReadOrdinals(JsonElement payload)
    {
        if (!payload.TryGetProperty("candidateOrdinals", out var ordinals) ||
            ordinals.ValueKind != JsonValueKind.Array)
        {
            throw Violation("InvalidPayload", "A started binding session must list its candidate ordinals.");
        }

        var values = new List<int>();
        foreach (var entry in ordinals.EnumerateArray())
        {
            if (entry.ValueKind != JsonValueKind.Number || !entry.TryGetInt32(out var value) || value < 0)
            {
                throw Violation("InvalidPayload", "A candidate ordinal is not a non-negative integer.");
            }

            values.Add(value);
        }

        return values.AsReadOnly();
    }

    private static string ReadSessionId(JsonElement payload) => ValidatedSessionId(ReadString(payload, "sessionId"));

    private static string RequireSessionMatch(JsonElement payload, string expectedSessionId)
    {
        var sessionId = ReadSessionId(payload);
        if (!string.Equals(sessionId, expectedSessionId, StringComparison.Ordinal))
        {
            throw Violation("SessionIdMismatch", "The response names a different binding session than the request.");
        }

        return sessionId;
    }

    private static DualBindingSessionState ReadState(JsonElement payload) =>
        Enum.TryParse<DualBindingSessionState>(ReadString(payload, "state"), out var state)
            ? state
            : throw Violation("InvalidPayload", "The binding state is not one this client understands.");

    private static int ReadOrdinal(JsonElement payload) => ValidatedOrdinal(ReadInt32(payload, "candidateOrdinal"));

    private static string ReadString(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : throw Violation("InvalidPayload", $"The Dual binding field '{name}' is missing or not a string.");

    private static bool ReadBoolean(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? value.GetBoolean()
            : throw Violation("InvalidPayload", $"The Dual binding field '{name}' is missing or not a boolean.");

    private static int ReadInt32(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt32(out var parsed)
            ? parsed
            : throw Violation("InvalidPayload", $"The Dual binding field '{name}' is missing or not an integer.");

    private static string ValidatedSessionId(string sessionId)
    {
        // Same shape the agent emits and accepts back: 32 hex characters, never all zero.
        if (sessionId.Length != 32 ||
            !sessionId.All(character => character is (>= '0' and <= '9') or (>= 'a' and <= 'f') or (>= 'A' and <= 'F')) ||
            sessionId.All(character => character == '0'))
        {
            throw Violation("InvalidSessionId", "A binding session ID must be 32 hex characters.");
        }

        return sessionId;
    }

    private static int ValidatedOrdinal(int candidateOrdinal) =>
        candidateOrdinal is >= 0 and < DualBindingCameraAgentProtocol.RequiredCandidateCount
            ? candidateOrdinal
            : throw Violation("UnknownCandidateOrdinal", "A candidate ordinal is outside this session's candidates.");

    private static void ValidateRequestId(string requestId)
    {
        if (string.IsNullOrEmpty(requestId) || requestId.Length > 128 ||
            !requestId.All(character =>
                char.IsAsciiLetterOrDigit(character) || character is '.' or '-' or '_'))
        {
            throw Violation("InvalidRequestId", "A bounded safe request ID is required.");
        }
    }

    private static HardwareProtocolViolationException Violation(
        string errorCode,
        string message,
        Exception? innerException = null) =>
        new(errorCode, message, innerException);
}
