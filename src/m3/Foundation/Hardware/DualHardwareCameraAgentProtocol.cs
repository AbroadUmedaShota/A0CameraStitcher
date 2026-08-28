using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using A0CameraStitcher.M3.Foundation.DualCamera;

namespace A0CameraStitcher.M3.Foundation.Hardware;

public static class DualHardwareCameraAgentProtocol
{
    public const string SchemaVersion = "a0.camera-agent.hardware-dual.v2";
    public const string Marker = "Hardware";
    public const string DefaultPipeName = "A0CameraStitcher.CameraAgent.HardwareDual.v2";

    public static class Operations
    {
        public const string GetCapabilities = "get-dual-capabilities";
        public const string ReservePairTransaction = "reserve-pair-transaction";
        public const string StartReservedPair = "start-reserved-pair";
        public const string StartReservedCaptureRecoveryOnly =
            "start-reserved-capture-recovery-only";
        public const string GetPairTransactionResult = "get-pair-transaction-result";
        public const string CloseReservedPairTransaction = "close-reserved-pair-transaction";

        public static IReadOnlyList<string> Required { get; } = Array.AsReadOnly([
            GetCapabilities,
            ReservePairTransaction,
            StartReservedPair,
            GetPairTransactionResult,
            CloseReservedPairTransaction,
        ]);
    }
}

public sealed record DualHardwareCameraAgentRequestEnvelope
{
    public required string SchemaVersion { get; init; }
    public required bool Simulation { get; init; }
    public required string Marker { get; init; }
    public required string RequestId { get; init; }
    public required string Operation { get; init; }
    public required JsonElement Payload { get; init; }
}

public sealed record DualHardwareCameraAgentResponseEnvelope
{
    public required string SchemaVersion { get; init; }
    public required bool Simulation { get; init; }
    public required string Marker { get; init; }
    public required string RequestId { get; init; }
    public required bool Success { get; init; }
    public required string ResultCode { get; init; }
    public required JsonElement Payload { get; init; }
}

public sealed record DualHardwareCapabilitiesRequest
{
    public required string CameraMode { get; init; }
}

public sealed record DualHardwareCapabilitiesResult
{
    public required string CameraMode { get; init; }
    public required int ProtocolVersion { get; init; }
    public required IReadOnlyList<string> OrderedRequiredAliases { get; init; }
    public required IReadOnlyList<string> SupportedOperations { get; init; }
    public required bool PairJournalDurable { get; init; }
    public required bool SameTransactionQueryOnly { get; init; }
    public required int AutomaticRetryCount { get; init; }
}

public sealed record DualHardwarePairReservationRequest
{
    public required string TransactionId { get; init; }
    public required string CameraMode { get; init; }
    public required IReadOnlyList<string> OrderedRequiredAliases { get; init; }
}

public sealed record DualHardwarePairReservationResult
{
    public required string TransactionId { get; init; }
    public required bool Accepted { get; init; }
}

public sealed record DualHardwarePairStartRequest
{
    public required string CameraMode { get; init; }
    public required IReadOnlyList<string> OrderedRequiredAliases { get; init; }
    public required DualHardwareCaptureRequest Transaction { get; init; }
}

public sealed record DualHardwareCaptureRecoveryOnlyPairStartRequest
{
    public required string CameraMode { get; init; }
    public required IReadOnlyList<string> OrderedRequiredAliases { get; init; }
    public required DualHardwareCaptureRecoveryOnlyRequest Transaction { get; init; }
}

public sealed record DualHardwarePairDispatchResult
{
    public required string TransactionId { get; init; }
    public required DualHardwareDispatchState DispatchState { get; init; }
    public required DualHardwareCaptureResult? Result { get; init; }
}

public sealed record DualHardwareCaptureRecoveryOnlyPairDispatchResult
{
    public required string TransactionId { get; init; }
    public required DualHardwareDispatchState DispatchState { get; init; }
    public required DualHardwareCaptureRecoveryOnlyResult? Result { get; init; }
}

public sealed record DualHardwarePairQueryRequest
{
    public required string TransactionId { get; init; }
}

public sealed record DualHardwarePairQueryResult
{
    public required string TransactionId { get; init; }
    public required bool Found { get; init; }
    public required DualHardwareCaptureResult? Result { get; init; }
}

public sealed record DualHardwareCaptureRecoveryOnlyPairQueryResult
{
    public required string TransactionId { get; init; }
    public required bool Found { get; init; }
    public required DualHardwareCaptureRecoveryOnlyResult? Result { get; init; }
}

public sealed record DualHardwarePairCloseRequest
{
    public required string TransactionId { get; init; }
}

public sealed record DualHardwarePairCloseResult
{
    public required string TransactionId { get; init; }
    public required bool ClosedBeforeDispatch { get; init; }
}

public static class DualHardwareCameraAgentProtocolCodec
{
    private const int MaximumProtocolJsonBytes = 256 * 1024;
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        RespectRequiredConstructorParameters = true,
        WriteIndented = false,
        MaxDepth = 32,
        Converters = { new JsonStringEnumConverter(), new DualHardwareTransactionIdConverter() },
    };

    public static DualHardwareCameraAgentRequestEnvelope CreateCapabilitiesRequest(
        string? requestId = null) =>
        CreateRequest(
            DualHardwareCameraAgentProtocol.Operations.GetCapabilities,
            new DualHardwareCapabilitiesRequest { CameraMode = "DualCamera" },
            requestId);

    public static DualHardwareCameraAgentRequestEnvelope CreateReservationRequest(
        Guid transactionId,
        string? requestId = null)
    {
        ValidateTransactionId(transactionId);
        return CreateRequest(
            DualHardwareCameraAgentProtocol.Operations.ReservePairTransaction,
            new DualHardwarePairReservationRequest
            {
                TransactionId = transactionId.ToString("N"),
                CameraMode = "DualCamera",
                OrderedRequiredAliases = OrderedAliases(),
            },
            requestId);
    }

    public static DualHardwareCameraAgentRequestEnvelope CreateStartRequest(
        DualHardwareCaptureRequest transaction,
        string? requestId = null)
    {
        ValidateCaptureRequest(transaction);
        return CreateRequest(
            DualHardwareCameraAgentProtocol.Operations.StartReservedPair,
            new DualHardwarePairStartRequest
            {
                CameraMode = "DualCamera",
                OrderedRequiredAliases = OrderedAliases(),
                Transaction = transaction,
            },
            requestId);
    }

    public static DualHardwareCameraAgentRequestEnvelope CreateCaptureRecoveryOnlyStartRequest(
        DualHardwareCaptureRecoveryOnlyRequest transaction,
        string? requestId = null)
    {
        ValidateCaptureRecoveryOnlyRequest(transaction);
        return CreateRequest(
            DualHardwareCameraAgentProtocol.Operations.StartReservedCaptureRecoveryOnly,
            new DualHardwareCaptureRecoveryOnlyPairStartRequest
            {
                CameraMode = "DualCamera",
                OrderedRequiredAliases = OrderedAliases(),
                Transaction = transaction,
            },
            requestId);
    }

    public static DualHardwareCameraAgentRequestEnvelope CreateQueryRequest(
        Guid transactionId,
        string? requestId = null)
    {
        ValidateTransactionId(transactionId);
        return CreateRequest(
            DualHardwareCameraAgentProtocol.Operations.GetPairTransactionResult,
            new DualHardwarePairQueryRequest { TransactionId = transactionId.ToString("N") },
            requestId);
    }

    public static DualHardwareCameraAgentRequestEnvelope CreateCloseRequest(
        Guid transactionId,
        string? requestId = null)
    {
        ValidateTransactionId(transactionId);
        return CreateRequest(
            DualHardwareCameraAgentProtocol.Operations.CloseReservedPairTransaction,
            new DualHardwarePairCloseRequest
            {
                TransactionId = transactionId.ToString("N"),
            },
            requestId);
    }

    public static string SerializeRequest(DualHardwareCameraAgentRequestEnvelope request)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateCommon(request.SchemaVersion, request.Simulation, request.Marker, request.RequestId);
        if (!DualHardwareCameraAgentProtocol.Operations.Required.Contains(request.Operation, StringComparer.Ordinal) &&
            request.Operation != DualHardwareCameraAgentProtocol.Operations.StartReservedCaptureRecoveryOnly)
            throw Violation("UnsupportedOperation", "The Dual hardware Agent operation is unsupported.");
        return JsonSerializer.Serialize(request, SerializerOptions);
    }

    public static DualHardwareCapabilitiesResult DeserializeCapabilitiesResponse(
        string json,
        string expectedRequestId)
    {
        var envelope = DeserializeResponse(json, expectedRequestId);
        if (!envelope.Success || envelope.ResultCode != "DualCapabilities")
            ThrowRemote(envelope);
        var payload = DeserializePayload<DualHardwareCapabilitiesResult>(envelope.Payload);
        if (payload.CameraMode != "DualCamera" || payload.ProtocolVersion != 2 ||
            !payload.OrderedRequiredAliases.SequenceEqual(OrderedAliases(), StringComparer.Ordinal) ||
            !SupportsRequiredOperations(payload.SupportedOperations) ||
            !payload.PairJournalDurable || !payload.SameTransactionQueryOnly || payload.AutomaticRetryCount != 0)
            throw Violation("UnsupportedDualCapabilities", "The Agent cannot safely own one durable no-retry pair transaction.");
        return payload;
    }

    private static bool SupportsRequiredOperations(IReadOnlyList<string> supportedOperations)
    {
        var requiredOperations = DualHardwareCameraAgentProtocol.Operations.Required;
        if (supportedOperations.Count < requiredOperations.Count ||
            supportedOperations.Count > 32)
            return false;

        var seen = new HashSet<string>(StringComparer.Ordinal);
        var requiredIndex = 0;
        foreach (var operation in supportedOperations)
        {
            if (string.IsNullOrWhiteSpace(operation) || operation.Length > 128 ||
                operation.Any(character =>
                    !((character >= 'a' && character <= 'z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-')) ||
                !seen.Add(operation))
                return false;

            if (requiredIndex < requiredOperations.Count &&
                operation == requiredOperations[requiredIndex])
                ++requiredIndex;
        }
        return requiredIndex == requiredOperations.Count;
    }

    public static bool DeserializeReservationResponse(
        string json,
        string expectedRequestId,
        Guid expectedTransactionId)
    {
        var envelope = DeserializeResponse(json, expectedRequestId);
        var payload = DeserializePayload<DualHardwarePairReservationResult>(envelope.Payload);
        ValidateTransactionMatch(payload.TransactionId, expectedTransactionId);
        if (envelope.Success)
        {
            if (envelope.ResultCode != "PairTransactionReserved" || !payload.Accepted)
                throw Violation("ForgedPairReservation", "A successful pair reservation response is inconsistent.");
            return true;
        }
        if (envelope.ResultCode == "DuplicateTransactionId" && !payload.Accepted)
            return false;
        ThrowRemote(envelope);
        return false;
    }

    public static DualHardwareDispatchResult DeserializeStartResponse(
        string json,
        string expectedRequestId,
        Guid expectedTransactionId)
    {
        var envelope = DeserializeResponse(json, expectedRequestId);
        if (!envelope.Success || envelope.ResultCode != "PairDispatchAccepted")
            ThrowRemote(envelope);
        var payload = DeserializePayload<DualHardwarePairDispatchResult>(envelope.Payload);
        ValidateTransactionMatch(payload.TransactionId, expectedTransactionId);
        if (payload.DispatchState is not (DualHardwareDispatchState.Completed or DualHardwareDispatchState.ResponseUnknown) ||
            (payload.DispatchState == DualHardwareDispatchState.Completed) != (payload.Result is not null) ||
            (payload.Result is not null && payload.Result.TransactionId != expectedTransactionId))
            throw Violation("InvalidPairDispatch", "The pair dispatch state and result are inconsistent.");
        return new(payload.DispatchState, payload.Result);
    }

    public static DualHardwareCaptureRecoveryOnlyDispatchResult DeserializeCaptureRecoveryOnlyStartResponse(
        string json,
        string expectedRequestId,
        Guid expectedTransactionId)
    {
        var envelope = DeserializeResponse(json, expectedRequestId);
        if (!envelope.Success || envelope.ResultCode != "PairDispatchAccepted")
            ThrowRemote(envelope);
        var payload = DeserializePayload<DualHardwareCaptureRecoveryOnlyPairDispatchResult>(envelope.Payload);
        ValidateTransactionMatch(payload.TransactionId, expectedTransactionId);
        if (payload.DispatchState is not (DualHardwareDispatchState.Completed or DualHardwareDispatchState.ResponseUnknown) ||
            (payload.DispatchState == DualHardwareDispatchState.Completed) != (payload.Result is not null) ||
            (payload.Result is not null && payload.Result.TransactionId != expectedTransactionId))
            throw Violation("InvalidPairDispatch", "The capture-recovery-only dispatch state and result are inconsistent.");
        if (payload.Result is not null) ValidateCaptureRecoveryOnlyResult(payload.Result);
        return new(payload.DispatchState, payload.Result);
    }

    public static DualHardwarePairQueryOutcome DeserializeQueryResponse(
        string json,
        string expectedRequestId,
        Guid expectedTransactionId)
    {
        var envelope = DeserializeResponse(json, expectedRequestId);
        var payload = DeserializePayload<DualHardwarePairQueryResult>(envelope.Payload);
        ValidateTransactionMatch(payload.TransactionId, expectedTransactionId);
        if (!envelope.Success)
        {
            if (envelope.ResultCode == "PairTransactionNotFound")
            {
                if (payload.Found || payload.Result is not null)
                    throw Violation("InvalidPairQuery", "A not-found pair query response is inconsistent.");
                return new(DualHardwarePairQueryState.NotFound, null);
            }
            if (envelope.ResultCode == "PairTransactionReserved")
            {
                if (!payload.Found || payload.Result is not null)
                    throw Violation("InvalidPairQuery", "A Reserved pair query response is inconsistent.");
                return new(DualHardwarePairQueryState.Reserved, null);
            }
            ThrowRemote(envelope);
        }
        if (envelope.ResultCode == "PairTransactionClosedBeforeDispatch")
        {
            if (!payload.Found || payload.Result is not null)
                throw Violation("InvalidPairQuery", "A ClosedBeforeDispatch pair query response is inconsistent.");
            return new(DualHardwarePairQueryState.ClosedBeforeDispatch, null);
        }
        if (envelope.ResultCode != "PairTransactionFound" || !payload.Found ||
            payload.Result is null || payload.Result.TransactionId != expectedTransactionId ||
            !IsActualTerminal(payload.Result.TerminalState))
            throw Violation("InvalidPairQuery", "The pair query response is inconsistent.");
        return new(DualHardwarePairQueryState.Terminal, payload.Result);
    }

    public static DualHardwareCaptureRecoveryOnlyPairQueryOutcome DeserializeCaptureRecoveryOnlyQueryResponse(
        string json,
        string expectedRequestId,
        Guid expectedTransactionId)
    {
        var envelope = DeserializeResponse(json, expectedRequestId);
        var payload = DeserializePayload<DualHardwareCaptureRecoveryOnlyPairQueryResult>(envelope.Payload);
        ValidateTransactionMatch(payload.TransactionId, expectedTransactionId);
        if (!envelope.Success)
        {
            if (envelope.ResultCode == "PairTransactionNotFound" && !payload.Found && payload.Result is null)
                return new(DualHardwarePairQueryState.NotFound, null);
            if (envelope.ResultCode == "PairTransactionReserved" && payload.Found && payload.Result is null)
                return new(DualHardwarePairQueryState.Reserved, null);
            ThrowRemote(envelope);
        }
        if (envelope.ResultCode == "PairTransactionClosedBeforeDispatch" && payload.Found && payload.Result is null)
            return new(DualHardwarePairQueryState.ClosedBeforeDispatch, null);
        if (envelope.ResultCode != "PairTransactionFound" || !payload.Found || payload.Result is null ||
            payload.Result.TransactionId != expectedTransactionId || !IsActualTerminal(payload.Result.TerminalState))
            throw Violation("InvalidPairQuery", "The capture-recovery-only pair query response is inconsistent.");
        ValidateCaptureRecoveryOnlyResult(payload.Result);
        return new(DualHardwarePairQueryState.Terminal, payload.Result);
    }

    public static DualHardwareCloseState DeserializeCloseResponse(
        string json,
        string expectedRequestId,
        Guid expectedTransactionId)
    {
        var envelope = DeserializeResponse(json, expectedRequestId);
        var payload = DeserializePayload<DualHardwarePairCloseResult>(envelope.Payload);
        ValidateTransactionMatch(payload.TransactionId, expectedTransactionId);
        if (!envelope.Success)
            ThrowRemote(envelope);
        if (envelope.ResultCode != "PairTransactionClosedBeforeDispatch" ||
            !payload.ClosedBeforeDispatch)
            throw Violation(
                "ForgedPairClose",
                "A successful pair close response did not confirm the durable ClosedBeforeDispatch tombstone.");
        return DualHardwareCloseState.ClosedBeforeDispatch;
    }

    private static DualHardwareCameraAgentRequestEnvelope CreateRequest<T>(
        string operation,
        T payload,
        string? requestId)
    {
        var resolvedRequestId = requestId ?? $"request-{Guid.NewGuid():N}";
        ValidateRequestId(resolvedRequestId);
        return new DualHardwareCameraAgentRequestEnvelope
        {
            SchemaVersion = DualHardwareCameraAgentProtocol.SchemaVersion,
            Simulation = false,
            Marker = DualHardwareCameraAgentProtocol.Marker,
            RequestId = resolvedRequestId,
            Operation = operation,
            Payload = JsonSerializer.SerializeToElement(payload, SerializerOptions),
        };
    }

    private static DualHardwareCameraAgentResponseEnvelope DeserializeResponse(
        string json,
        string expectedRequestId)
    {
        ValidateJson(json);
        DualHardwareCameraAgentResponseEnvelope envelope;
        try
        {
            envelope = JsonSerializer.Deserialize<DualHardwareCameraAgentResponseEnvelope>(json, SerializerOptions)
                ?? throw Violation("MalformedEnvelope", "The Dual hardware Agent response is empty.");
        }
        catch (HardwareProtocolViolationException)
        {
            throw;
        }
        catch (JsonException exception)
        {
            throw Violation("MalformedEnvelope", "The Dual hardware Agent response is malformed.", exception);
        }
        ValidateCommon(envelope.SchemaVersion, envelope.Simulation, envelope.Marker, envelope.RequestId);
        if (!string.Equals(envelope.RequestId, expectedRequestId, StringComparison.Ordinal))
            throw Violation("RequestIdMismatch", "The Dual hardware Agent response does not match the request.");
        return envelope;
    }

    private static T DeserializePayload<T>(JsonElement payload)
    {
        if (payload.ValueKind != JsonValueKind.Object)
            throw Violation("InvalidPayload", "The Dual hardware Agent payload must be an object.");
        try
        {
            return payload.Deserialize<T>(SerializerOptions)
                ?? throw Violation("InvalidPayload", "The Dual hardware Agent payload is empty.");
        }
        catch (HardwareProtocolViolationException)
        {
            throw;
        }
        catch (JsonException exception)
        {
            throw Violation("InvalidPayload", "The Dual hardware Agent payload is malformed.", exception);
        }
    }

    private static void ValidateCaptureRequest(DualHardwareCaptureRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateTransactionId(request.TransactionId);
        if (!request.IdentitySnapshot.IsReady || request.OperatorConfirmations.AllConfirmed != true ||
            request.StartedAtUtc.Offset != TimeSpan.Zero || request.WatchdogDeadlineUtc.Offset != TimeSpan.Zero ||
            request.WatchdogDeadlineUtc - request.StartedAtUtc != TimeSpan.FromSeconds(180) ||
            string.IsNullOrWhiteSpace(request.TransactionDirectory) ||
            !Path.IsPathFullyQualified(request.TransactionDirectory))
            throw Violation("InvalidPairRequest", "The frozen Dual hardware transaction is incomplete or unsafe.");
        request.CaptureProfileSnapshot.Validate(request.StartedAtUtc);
        request.RigProfileSnapshot.Validate(request.StartedAtUtc);
    }

    private static void ValidateCaptureRecoveryOnlyRequest(DualHardwareCaptureRecoveryOnlyRequest request)
    {
        ArgumentNullException.ThrowIfNull(request);
        ValidateTransactionId(request.TransactionId);
        if (!request.IdentitySnapshot.IsReady || request.OperatorConfirmations.AllConfirmed != true ||
            request.StartedAtUtc.Offset != TimeSpan.Zero || request.WatchdogDeadlineUtc.Offset != TimeSpan.Zero ||
            request.WatchdogDeadlineUtc - request.StartedAtUtc != TimeSpan.FromSeconds(180) ||
            string.IsNullOrWhiteSpace(request.TransactionDirectory) ||
            !Path.IsPathFullyQualified(request.TransactionDirectory))
            throw Violation("InvalidPairRequest", "The capture-recovery-only transaction is incomplete or unsafe.");
        request.CaptureProfileSnapshot.Validate();
    }

    private static void ValidateCaptureRecoveryOnlyResult(DualHardwareCaptureRecoveryOnlyResult result)
    {
        if (result.CapturePurpose != "CaptureRecoveryOnly" || result.StitchOutcome != "Pending" ||
            result.A0QualityApproval != "Unapproved" || result.Evidence.TerminalState != result.TerminalState ||
            result.Evidence.AutomaticRetryCount != 0 ||
            !string.Equals(
                result.Evidence.CaptureProfileSchemaVersion,
                "a0.dual-capture-profile.operator-approved.v1",
                StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(result.Evidence.CaptureProfileApprovalBasis) ||
            result.Evidence.CaptureProfileApprovalBasis.Length > 128 ||
            result.Evidence.CaptureProfileApprovalBasis.Any(char.IsControl) ||
            !string.Equals(result.Evidence.CameraModel, "Nikon D810", StringComparison.Ordinal) ||
            !string.Equals(result.Evidence.ImageFormat, "JPEG Fine", StringComparison.Ordinal) ||
            !string.Equals(result.Evidence.ImageSize, "L", StringComparison.Ordinal) ||
            !string.Equals(result.Evidence.PixelDimensions, "7360x4912", StringComparison.Ordinal))
            throw Violation("InvalidCaptureRecoveryOnlyResult", "The recovery-only terminal result claims unsupported rig or quality evidence.");
    }

    private static void ValidateJson(string json)
    {
        if (string.IsNullOrEmpty(json) || Encoding.UTF8.GetByteCount(json) > MaximumProtocolJsonBytes)
            throw Violation("MalformedEnvelope", "The Dual hardware Agent JSON size is invalid.");
        try
        {
            using var document = JsonDocument.Parse(json, new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 32,
            });
            RejectDuplicateProperties(document.RootElement);
        }
        catch (HardwareProtocolViolationException)
        {
            throw;
        }
        catch (JsonException exception)
        {
            throw Violation("MalformedEnvelope", "The Dual hardware Agent JSON is malformed.", exception);
        }
    }

    private static void RejectDuplicateProperties(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in element.EnumerateObject())
            {
                if (!names.Add(property.Name))
                    throw Violation("DuplicateField", "The Dual hardware Agent JSON contains a duplicate field.");
                RejectDuplicateProperties(property.Value);
            }
        }
        else if (element.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in element.EnumerateArray()) RejectDuplicateProperties(item);
        }
    }

    private static void ValidateCommon(string schemaVersion, bool simulation, string marker, string requestId)
    {
        if (schemaVersion != DualHardwareCameraAgentProtocol.SchemaVersion || simulation ||
            marker != DualHardwareCameraAgentProtocol.Marker)
            throw Violation("DualHardwareProtocolRequired", "The response is not the required Dual hardware v2 protocol.");
        ValidateRequestId(requestId);
    }

    private static void ValidateRequestId(string requestId)
    {
        if (!IsSafeToken(requestId, 128))
            throw Violation("InvalidRequestId", "A bounded safe request ID is required.");
    }

    private static void ValidateTransactionId(Guid transactionId)
    {
        if (transactionId == Guid.Empty)
            throw Violation("InvalidTransactionId", "A non-empty pair transaction ID is required.");
    }

    private static void ValidateTransactionMatch(string transactionId, Guid expected)
    {
        if (!Guid.TryParseExact(transactionId, "N", out var parsed) || parsed != expected)
            throw Violation("TransactionIdMismatch", "The Dual hardware Agent response belongs to another transaction.");
    }

    private static IReadOnlyList<string> OrderedAliases() => Array.AsReadOnly(["CAM-A", "CAM-B"]);

    private static bool IsActualTerminal(DualHardwareCaptureTerminalState state) => state is
        DualHardwareCaptureTerminalState.Succeeded or
        DualHardwareCaptureTerminalState.Failed or
        DualHardwareCaptureTerminalState.FailedPartial or
        DualHardwareCaptureTerminalState.WatchdogExpired;

    private static bool IsSafeToken(string? value, int maximumLength) =>
        !string.IsNullOrEmpty(value) && value.Length <= maximumLength && value.All(character =>
            character is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9' or '.' or '-' or '_');

    private static void ThrowRemote(DualHardwareCameraAgentResponseEnvelope envelope)
    {
        throw new HardwareCameraAgentRemoteException(
            envelope.RequestId,
            envelope.ResultCode,
            envelope.ResultCode,
            "Dual hardware Camera Agent rejected the request.");
    }

    private static HardwareProtocolViolationException Violation(
        string errorCode,
        string message,
        Exception? innerException = null) => new(errorCode, message, innerException);
}

internal sealed class DualHardwareTransactionIdConverter : JsonConverter<Guid>
{
    public override Guid Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.String ||
            !Guid.TryParseExact(reader.GetString(), "N", out var transactionId) ||
            transactionId == Guid.Empty)
            throw new JsonException("A Dual hardware transaction ID must contain 32 hexadecimal characters.");
        return transactionId;
    }

    public override void Write(Utf8JsonWriter writer, Guid value, JsonSerializerOptions options)
    {
        if (value == Guid.Empty)
            throw new JsonException("A Dual hardware transaction ID cannot be empty.");
        writer.WriteStringValue(value.ToString("N"));
    }
}

public sealed class DualHardwareCameraAgentOperations : IDualHardwareCaptureOperations, IDualHardwareCaptureRecoveryOnlyOperations
{
    private readonly IHardwareCameraAgentTransport _transport;
    private readonly SemaphoreSlim _capabilityLock = new(1, 1);
    private DualHardwareCapabilitiesResult? _capabilities;

    public DualHardwareCameraAgentOperations(IHardwareCameraAgentTransport transport)
    {
        _transport = transport ?? throw new ArgumentNullException(nameof(transport));
    }

    public DualHardwareCameraAgentOperations(
        string pipeName,
        int expectedServerProcessId,
        TimeSpan? connectTimeout = null,
        TimeSpan? responseTimeout = null)
        : this(new NamedPipeHardwareCameraAgentTransport(
            pipeName, expectedServerProcessId, connectTimeout, responseTimeout))
    {
    }

    public async Task<bool> ReservePairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        await EnsureCapabilitiesAsync(cancellationToken).ConfigureAwait(false);
        var request = DualHardwareCameraAgentProtocolCodec.CreateReservationRequest(transactionId);
        var response = await _transport.SendAsync(
            DualHardwareCameraAgentProtocolCodec.SerializeRequest(request),
            cancellationToken).ConfigureAwait(false);
        return DualHardwareCameraAgentProtocolCodec.DeserializeReservationResponse(
            response,
            request.RequestId,
            transactionId);
    }

    public async Task<DualHardwareDispatchResult> StartReservedPairAsync(
        DualHardwareCaptureRequest request,
        CancellationToken cancellationToken)
    {
        await EnsureCapabilitiesAsync(cancellationToken).ConfigureAwait(false);
        var envelope = DualHardwareCameraAgentProtocolCodec.CreateStartRequest(request);
        var response = await _transport.SendAsync(
            DualHardwareCameraAgentProtocolCodec.SerializeRequest(envelope),
            cancellationToken).ConfigureAwait(false);
        return DualHardwareCameraAgentProtocolCodec.DeserializeStartResponse(
            response,
            envelope.RequestId,
            request.TransactionId);
    }

    public async Task<DualHardwareCaptureRecoveryOnlyDispatchResult> StartReservedCaptureRecoveryOnlyAsync(
        DualHardwareCaptureRecoveryOnlyRequest request,
        CancellationToken cancellationToken)
    {
        await EnsureCaptureRecoveryOnlyAvailableAsync(cancellationToken).ConfigureAwait(false);
        var envelope = DualHardwareCameraAgentProtocolCodec.CreateCaptureRecoveryOnlyStartRequest(request);
        var response = await _transport.SendAsync(
            DualHardwareCameraAgentProtocolCodec.SerializeRequest(envelope),
            cancellationToken).ConfigureAwait(false);
        return DualHardwareCameraAgentProtocolCodec.DeserializeCaptureRecoveryOnlyStartResponse(
            response, envelope.RequestId, request.TransactionId);
    }

    public async Task<DualHardwarePairQueryOutcome> QueryPairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        await EnsureCapabilitiesAsync(cancellationToken).ConfigureAwait(false);
        var request = DualHardwareCameraAgentProtocolCodec.CreateQueryRequest(transactionId);
        var response = await _transport.SendAsync(
            DualHardwareCameraAgentProtocolCodec.SerializeRequest(request),
            cancellationToken).ConfigureAwait(false);
        return DualHardwareCameraAgentProtocolCodec.DeserializeQueryResponse(
            response,
            request.RequestId,
            transactionId);
    }

    public async Task<DualHardwareCaptureRecoveryOnlyPairQueryOutcome> QueryCaptureRecoveryOnlyTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        await EnsureCaptureRecoveryOnlyAvailableAsync(cancellationToken).ConfigureAwait(false);
        var request = DualHardwareCameraAgentProtocolCodec.CreateQueryRequest(transactionId);
        var response = await _transport.SendAsync(
            DualHardwareCameraAgentProtocolCodec.SerializeRequest(request),
            cancellationToken).ConfigureAwait(false);
        return DualHardwareCameraAgentProtocolCodec.DeserializeCaptureRecoveryOnlyQueryResponse(
            response, request.RequestId, transactionId);
    }

    public async Task<DualHardwareCloseState> CloseReservedPairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        await EnsureCapabilitiesAsync(cancellationToken).ConfigureAwait(false);
        var request = DualHardwareCameraAgentProtocolCodec.CreateCloseRequest(transactionId);
        var response = await _transport.SendAsync(
            DualHardwareCameraAgentProtocolCodec.SerializeRequest(request),
            cancellationToken).ConfigureAwait(false);
        return DualHardwareCameraAgentProtocolCodec.DeserializeCloseResponse(
            response,
            request.RequestId,
            transactionId);
    }

    public async Task EnsureCaptureRecoveryOnlyAvailableAsync(CancellationToken cancellationToken)
    {
        var capabilities = await EnsureCapabilitiesAsync(cancellationToken).ConfigureAwait(false);
        if (!capabilities.SupportedOperations.Contains(
                DualHardwareCameraAgentProtocol.Operations.StartReservedCaptureRecoveryOnly,
                StringComparer.Ordinal))
            throw new HardwareProtocolViolationException(
                "CaptureRecoveryOnlyUnsupported",
                "The Dual hardware Agent does not advertise CaptureRecoveryOnly.");
    }

    private async Task<DualHardwareCapabilitiesResult> EnsureCapabilitiesAsync(CancellationToken cancellationToken)
    {
        if (_capabilities is not null) return _capabilities;
        await _capabilityLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_capabilities is not null) return _capabilities;
            var request = DualHardwareCameraAgentProtocolCodec.CreateCapabilitiesRequest();
            var response = await _transport.SendAsync(
                DualHardwareCameraAgentProtocolCodec.SerializeRequest(request),
                cancellationToken).ConfigureAwait(false);
            _capabilities = DualHardwareCameraAgentProtocolCodec.DeserializeCapabilitiesResponse(
                response,
                request.RequestId);
            return _capabilities;
        }
        finally
        {
            _capabilityLock.Release();
        }
    }
}
