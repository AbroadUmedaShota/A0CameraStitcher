using System.Text.Json;
using System.Text.Json.Serialization;

namespace A0CameraStitcher.M3.Foundation;

public static class CameraAgentProtocol
{
    public const string SchemaVersion = "a0.camera-agent.simulated.v1";
    public const string Marker = "Simulated";

    public static class Operations
    {
        public const string Status = "status";
        public const string Inventory = "inventory";
        public const string PreviewPlaceholder = "preview-placeholder";

        public static bool IsSupported(string operation) =>
            operation is Status or Inventory or PreviewPlaceholder;
    }
}

public sealed record CameraAgentRequestEnvelope
{
    public required string SchemaVersion { get; init; }

    public required bool Simulation { get; init; }

    public required string Marker { get; init; }

    public required string RequestId { get; init; }

    public required string Operation { get; init; }

    public required JsonElement Payload { get; init; }
}

public sealed record CameraAgentResponseEnvelope
{
    public required string SchemaVersion { get; init; }

    public required bool Simulation { get; init; }

    public required string Marker { get; init; }

    public required string RequestId { get; init; }

    public required bool Success { get; init; }

    public required string ResultCode { get; init; }

    public required JsonElement Payload { get; init; }
}

public sealed class ProtocolViolationException : Exception
{
    public ProtocolViolationException(string errorCode, string message, Exception? innerException = null)
        : base(message, innerException)
    {
        ErrorCode = errorCode;
    }

    public string ErrorCode { get; }
}

public static class CameraAgentProtocolCodec
{
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        RespectRequiredConstructorParameters = true,
        WriteIndented = false,
    };

    public static CameraAgentRequestEnvelope CreateRequest(
        string operation,
        string? requestId = null,
        JsonElement? payload = null)
    {
        var envelope = new CameraAgentRequestEnvelope
        {
            SchemaVersion = CameraAgentProtocol.SchemaVersion,
            Simulation = true,
            Marker = CameraAgentProtocol.Marker,
            RequestId = requestId ?? Guid.NewGuid().ToString("N"),
            Operation = operation,
            Payload = payload ?? EmptyPayload(),
        };

        ValidateRequest(envelope);
        return envelope;
    }

    public static CameraAgentResponseEnvelope CreateSuccess(
        string requestId,
        string resultCode,
        JsonElement payload)
    {
        var envelope = new CameraAgentResponseEnvelope
        {
            SchemaVersion = CameraAgentProtocol.SchemaVersion,
            Simulation = true,
            Marker = CameraAgentProtocol.Marker,
            RequestId = requestId,
            Success = true,
            ResultCode = resultCode,
            Payload = payload,
        };

        ValidateResponse(envelope);
        return envelope;
    }

    public static CameraAgentResponseEnvelope CreateRejection(
        string requestId,
        string rejectionCode)
    {
        var safeRequestId = IsValidRequestId(requestId) ? requestId : "rejected";
        var payload = JsonSerializer.SerializeToElement(
            new
            {
                marker = CameraAgentProtocol.Marker,
                cameraAccess = "None",
                rejectionCode,
            },
            SerializerOptions);

        var envelope = new CameraAgentResponseEnvelope
        {
            SchemaVersion = CameraAgentProtocol.SchemaVersion,
            Simulation = true,
            Marker = CameraAgentProtocol.Marker,
            RequestId = safeRequestId,
            Success = false,
            ResultCode = "ProtocolRejected",
            Payload = payload,
        };

        ValidateResponse(envelope);
        return envelope;
    }

    public static string SerializeRequest(CameraAgentRequestEnvelope envelope)
    {
        ValidateRequest(envelope);
        return JsonSerializer.Serialize(envelope, SerializerOptions);
    }

    public static string SerializeResponse(CameraAgentResponseEnvelope envelope)
    {
        ValidateResponse(envelope);
        return JsonSerializer.Serialize(envelope, SerializerOptions);
    }

    public static CameraAgentRequestEnvelope DeserializeRequest(string json)
    {
        CameraAgentRequestEnvelope envelope;
        try
        {
            envelope = JsonSerializer.Deserialize<CameraAgentRequestEnvelope>(json, SerializerOptions)
                ?? throw new ProtocolViolationException("MalformedEnvelope", "Request envelope was null.");
        }
        catch (ProtocolViolationException)
        {
            throw;
        }
        catch (JsonException exception)
        {
            throw new ProtocolViolationException("MalformedEnvelope", "Request envelope is not valid protocol JSON.", exception);
        }

        ValidateRequest(envelope);
        return envelope;
    }

    public static CameraAgentResponseEnvelope DeserializeResponse(string json)
    {
        CameraAgentResponseEnvelope envelope;
        try
        {
            envelope = JsonSerializer.Deserialize<CameraAgentResponseEnvelope>(json, SerializerOptions)
                ?? throw new ProtocolViolationException("MalformedEnvelope", "Response envelope was null.");
        }
        catch (ProtocolViolationException)
        {
            throw;
        }
        catch (JsonException exception)
        {
            throw new ProtocolViolationException("MalformedEnvelope", "Response envelope is not valid protocol JSON.", exception);
        }

        ValidateResponse(envelope);
        return envelope;
    }

    internal static string TryExtractRequestId(string json)
    {
        try
        {
            using var document = JsonDocument.Parse(json);
            return document.RootElement.TryGetProperty("requestId", out var property) &&
                property.ValueKind == JsonValueKind.String &&
                IsValidRequestId(property.GetString())
                ? property.GetString()!
                : "rejected";
        }
        catch (JsonException)
        {
            return "rejected";
        }
    }

    private static void ValidateRequest(CameraAgentRequestEnvelope envelope)
    {
        ValidateCommon(envelope.SchemaVersion, envelope.Simulation, envelope.Marker, envelope.RequestId);
        if (!CameraAgentProtocol.Operations.IsSupported(envelope.Operation))
        {
            throw new ProtocolViolationException("UnsupportedOperation", "The request operation is not supported.");
        }

        ValidatePayload(envelope.Payload);
    }

    private static void ValidateResponse(CameraAgentResponseEnvelope envelope)
    {
        ValidateCommon(envelope.SchemaVersion, envelope.Simulation, envelope.Marker, envelope.RequestId);
        if (string.IsNullOrWhiteSpace(envelope.ResultCode))
        {
            throw new ProtocolViolationException("MissingResultCode", "A result code is required.");
        }

        ValidatePayload(envelope.Payload);
    }

    private static void ValidateCommon(string schemaVersion, bool simulation, string marker, string requestId)
    {
        if (!string.Equals(schemaVersion, CameraAgentProtocol.SchemaVersion, StringComparison.Ordinal))
        {
            throw new ProtocolViolationException("UnsupportedSchemaVersion", "The protocol schema version is not supported.");
        }

        if (!simulation)
        {
            throw new ProtocolViolationException("SimulationRequired", "Only explicitly simulated envelopes are accepted.");
        }

        if (!string.Equals(marker, CameraAgentProtocol.Marker, StringComparison.Ordinal))
        {
            throw new ProtocolViolationException("SimulationMarkerRequired", "The Simulated marker is required.");
        }

        if (!IsValidRequestId(requestId))
        {
            throw new ProtocolViolationException("InvalidRequestId", "A bounded request ID is required.");
        }
    }

    private static bool IsValidRequestId(string? requestId) =>
        !string.IsNullOrWhiteSpace(requestId) && requestId.Length <= 128;

    private static void ValidatePayload(JsonElement payload)
    {
        if (payload.ValueKind != JsonValueKind.Object)
        {
            throw new ProtocolViolationException("InvalidPayload", "The envelope payload must be a JSON object.");
        }
    }

    private static JsonElement EmptyPayload() => JsonSerializer.SerializeToElement(new { }, SerializerOptions);
}
