using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace A0CameraStitcher.M3.Foundation.Hardware;

public static class HardwareCameraAgentProtocol
{
    public const string SchemaVersion = "a0.camera-agent.hardware.v1";
    public const string Marker = "Hardware";
    public const string DefaultPipeName = "A0CameraStitcher.CameraAgent.Hardware.v1";

    public static class Operations
    {
        public const string GetSingleReadiness = "get-single-readiness";
        public const string CaptureSingle = "capture-single";
        public const string LiveViewProbe = "live-view-probe";
        public const string GetTransactionResult = "get-transaction-result";

        public static bool IsSupported(string? operation) => operation is
            GetSingleReadiness or CaptureSingle or LiveViewProbe or GetTransactionResult;
    }
}

public sealed record HardwareCameraAgentRequestEnvelope
{
    public required string SchemaVersion { get; init; }

    public required bool Simulation { get; init; }

    public required string Marker { get; init; }

    public required string RequestId { get; init; }

    public required string Operation { get; init; }

    public required JsonElement Payload { get; init; }
}

public sealed record HardwareCameraAgentResponseEnvelope
{
    public required string SchemaVersion { get; init; }

    public required bool Simulation { get; init; }

    public required string Marker { get; init; }

    public required string RequestId { get; init; }

    public required bool Success { get; init; }

    public required string ResultCode { get; init; }

    public required JsonElement Payload { get; init; }
}

public sealed record HardwareReadinessRequest
{
    public required string CameraAlias { get; init; }
}

public sealed record HardwareCaptureSingleRequest
{
    public required string TransactionId { get; init; }

    public required string CameraAlias { get; init; }

    public required string ExpectedCaptureProfileId { get; init; }

    public required uint ExpectedCaptureProfileVersion { get; init; }

    public required string ExpectedCaptureProfileSha256 { get; init; }

    public required string ExpectedCaptureProfileExpiresAtUtc { get; init; }

    public required bool ExclusiveCameraControlConfirmed { get; init; }

    public required bool DedicatedSpoolScopeConfirmed { get; init; }

    public required bool ExactObjectDeleteConfirmed { get; init; }

    public required bool LiveViewHandoffRequested { get; init; }
}

public sealed record HardwareLiveViewProbeRequest
{
    public required string CameraAlias { get; init; }

    public required bool ExclusiveCameraControlConfirmed { get; init; }

    public required int LiveViewFrames { get; init; }

    public required int LiveViewIntervalMs { get; init; }
}

public sealed record HardwareTransactionResultRequest
{
    public required string TransactionId { get; init; }
}

public sealed record HardwareCaptureSafetyConfirmations(
    bool ExclusiveCameraControlConfirmed,
    bool DedicatedSpoolScopeConfirmed,
    bool ExactObjectDeleteConfirmed)
{
    public static HardwareCaptureSafetyConfirmations AllConfirmed { get; } = new(true, true, true);
}

public sealed record HardwareCaptureProfileSnapshot(
    string ProfileId,
    uint ProfileVersion,
    string Sha256,
    DateTimeOffset ExpiresAtUtc);

public sealed record HardwareLiveViewSafetyConfirmation(bool ExclusiveCameraControlConfirmed)
{
    public static HardwareLiveViewSafetyConfirmation Confirmed { get; } = new(true);
}

public sealed record HardwareSingleReadinessResult
{
    public required string CameraMode { get; init; }

    public required string CameraAlias { get; init; }

    public required bool Ready { get; init; }

    public required int SdkCameraCount { get; init; }

    public required int WpdCameraCount { get; init; }

    public required bool SdkIdentityBound { get; init; }

    public required bool WpdIdentityBound { get; init; }

    public required bool SdkAliasMatches { get; init; }

    public required bool WpdAliasMatches { get; init; }

    public required bool SdkStatusProbed { get; init; }

    public required bool SpoolInspected { get; init; }

    public required long SpoolPayloadObjectCount { get; init; }

    public required bool SpoolKnownEmpty { get; init; }

    public required string Firmware { get; init; }

    public required string LiveViewStatus { get; init; }

    public required bool LiveViewStatusAvailable { get; init; }

    public required bool CaptureProfileApproved { get; init; }

    public required string CaptureProfileId { get; init; }

    public required uint CaptureProfileVersion { get; init; }

    public required string CaptureProfileSha256 { get; init; }

    public required string CaptureProfileCameraAlias { get; init; }

    [JsonPropertyName("profileExpiresAtUtc")]
    [JsonConverter(typeof(HardwareOptionalUtcTimestampConverter))]
    public required DateTimeOffset? CaptureProfileExpiresAtUtc { get; init; }

    public required bool CaptureProfileAliasMatches { get; init; }

    public required bool SettingsMatchApprovedProfile { get; init; }

    public required HardwareObservedCameraSettings ObservedSettings { get; init; }

    public required bool ReadOnly { get; init; }

    public required bool CaptureCommandSent { get; init; }

    public required bool CameraObjectDeleteAttempted { get; init; }

    public required bool CameraSettingsChanged { get; init; }

    public required bool RealIdentifiersIncluded { get; init; }

    public required string FailureCategory { get; init; }

    public required string FailureDetail { get; init; }
}

public sealed record HardwareObservedCameraSettings
{
    public required HardwareObservedCameraSetting FileType { get; init; }

    public required HardwareObservedCameraSetting CompressionLevel { get; init; }

    public required HardwareObservedCameraSetting ImageSize { get; init; }

    public required HardwareObservedCameraSetting ExposureMode { get; init; }

    public required HardwareObservedCameraSetting ShutterSpeed { get; init; }

    public required HardwareObservedCameraSetting Aperture { get; init; }

    public required HardwareObservedCameraSetting Sensitivity { get; init; }

    public required HardwareObservedCameraSetting WhiteBalanceMode { get; init; }

    public required HardwareObservedCameraSetting FocusMode { get; init; }
}

public sealed record HardwareObservedCameraSetting
{
    public required bool Available { get; init; }

    public required string CapType { get; init; }

    public required string ProbeState { get; init; }

    public required string ValueType { get; init; }

    public required uint? CurrentValue { get; init; }

    public required uint? CurrentIndex { get; init; }

    public required string? CurrentLabel { get; init; }
}

public sealed record HardwareRetainedOriginalRecord
{
    public required string CameraAlias { get; init; }

    public required string Path { get; init; }

    public required long SizeBytes { get; init; }

    public required string Sha256 { get; init; }
}

public sealed record HardwareSingleCaptureResult
{
    public required string CameraMode { get; init; }

    public required string CameraAlias { get; init; }

    public required string RequiredCameraAlias { get; init; }

    public required string RunId { get; init; }

    public required string TransactionId { get; init; }

    public required string CaptureProfileId { get; init; }

    public required uint CaptureProfileVersion { get; init; }

    public required string CaptureProfileSha256 { get; init; }

    public required string CaptureProfileCameraAlias { get; init; }

    [JsonPropertyName("profileExpiresAtUtc")]
    [JsonConverter(typeof(HardwareOptionalUtcTimestampConverter))]
    public required DateTimeOffset? CaptureProfileExpiresAtUtc { get; init; }

    public required string TerminalState { get; init; }

    public required string ErrorCategory { get; init; }

    public required string ErrorDetail { get; init; }

    public required HardwareRetainedOriginalRecord? RetainedOriginal { get; init; }

    public required bool LiveViewHandoffRequested { get; init; }

    public required bool LiveViewStoppedBeforeCapture { get; init; }

    public required bool LiveViewSdkSessionClosedBeforeCapture { get; init; }

    public required bool PostCaptureLiveViewProbeAttempted { get; init; }

    public required bool PostCaptureLiveViewProbeSucceeded { get; init; }

    public required HardwarePreviewJpegRecord? PostCapturePreview { get; init; }

    public required bool SpoolEmptyBeforeCapture { get; init; }

    public required bool CameraObjectDeleteAttempted { get; init; }

    public required bool CameraObjectDeleteSucceeded { get; init; }

    public required bool SpoolEmptyAfterCleanup { get; init; }

    public required int AutomaticRetryCount { get; init; }

    public required int TransactionWatchdogSeconds { get; init; }

    public required bool RealIdentifiersIncluded { get; init; }
}

public sealed record HardwarePreviewJpegRecord
{
    public required string Path { get; init; }

    public required long SizeBytes { get; init; }

    public required string Sha256 { get; init; }
}

public sealed record HardwareSingleLiveViewResult
{
    public required string CameraMode { get; init; }

    public required string CameraAlias { get; init; }

    public required string RunId { get; init; }

    public required int Frames { get; init; }

    public required long LastFrameBytes { get; init; }

    public required string LastFrameSha256 { get; init; }

    public required long DurationMs { get; init; }

    public required bool PreviewPersisted { get; init; }

    public required HardwarePreviewJpegRecord? Preview { get; init; }

    public required bool PreviewIsOriginal { get; init; }

    public required bool PreviewIsStitchInput { get; init; }

    public required bool LiveViewStopped { get; init; }

    public required bool SdkSessionClosed { get; init; }

    public required bool RealIdentifiersIncluded { get; init; }

    public required string ErrorCategory { get; init; }

    public required string ErrorDetail { get; init; }
}

public sealed record HardwareCameraAgentFailure
{
    public required string CameraAccess { get; init; }

    public required string RejectionCode { get; init; }

    public required string ErrorDetail { get; init; }

    public required bool RealIdentifiersIncluded { get; init; }
}

public sealed record HardwareCameraAgentReply<T>(
    string RequestId,
    bool Success,
    string ResultCode,
    T Payload);

public sealed class HardwareProtocolViolationException : Exception
{
    public HardwareProtocolViolationException(
        string errorCode,
        string message,
        Exception? innerException = null)
        : base(message, innerException)
    {
        ErrorCode = errorCode;
    }

    public string ErrorCode { get; }
}

public sealed class HardwareCameraAgentRemoteException : Exception
{
    public HardwareCameraAgentRemoteException(
        string requestId,
        string resultCode,
        string rejectionCode,
        string errorDetail)
        : base(string.IsNullOrEmpty(errorDetail)
            ? $"Hardware Camera Agent rejected the request ({resultCode})."
            : $"Hardware Camera Agent rejected the request ({resultCode}): {errorDetail}")
    {
        RequestId = requestId;
        ResultCode = resultCode;
        RejectionCode = rejectionCode;
        ErrorDetail = errorDetail;
    }

    public string RequestId { get; }

    public string ResultCode { get; }

    public string RejectionCode { get; }

    public string ErrorDetail { get; }
}

public sealed class HardwareOptionalUtcTimestampConverter : JsonConverter<DateTimeOffset?>
{
    private const string WireFormat = "yyyy-MM-dd'T'HH:mm:ss'Z'";

    public override DateTimeOffset? Read(
        ref Utf8JsonReader reader,
        Type typeToConvert,
        JsonSerializerOptions options)
    {
        if (reader.TokenType != JsonTokenType.String)
        {
            throw new JsonException("A hardware profile expiry must be an ISO UTC string.");
        }

        var value = reader.GetString();
        if (string.IsNullOrEmpty(value))
        {
            return null;
        }

        if (!DateTimeOffset.TryParseExact(
                value,
                WireFormat,
                CultureInfo.InvariantCulture,
                DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal,
                out var parsed))
        {
            throw new JsonException("A hardware profile expiry must use second-precision UTC format.");
        }

        return parsed;
    }

    public override void Write(
        Utf8JsonWriter writer,
        DateTimeOffset? value,
        JsonSerializerOptions options) =>
        writer.WriteStringValue(value.HasValue
            ? value.Value.ToUniversalTime().ToString(WireFormat, CultureInfo.InvariantCulture)
            : string.Empty);
}

public static class HardwareCameraAgentProtocolCodec
{
    private const int MaximumProtocolJsonBytes = 64 * 1024;
    private const int MaximumErrorDetailLength = 512;
    private const long MaximumVerifiedJpegBytes = 256L * 1024L * 1024L;
    private const string ProfileUtcWireFormat = "yyyy-MM-dd'T'HH:mm:ss'Z'";

    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        RespectRequiredConstructorParameters = true,
        WriteIndented = false,
        MaxDepth = 8,
    };

    public static string CreateTransactionId() => Guid.NewGuid().ToString("N");

    public static HardwareCameraAgentRequestEnvelope CreateReadinessRequest(
        string cameraAlias,
        string? requestId = null) =>
        CreateRequest(
            HardwareCameraAgentProtocol.Operations.GetSingleReadiness,
            new HardwareReadinessRequest { CameraAlias = cameraAlias },
            requestId);

    public static HardwareCameraAgentRequestEnvelope CreateCaptureRequest(
        string transactionId,
        string cameraAlias,
        HardwareCaptureSafetyConfirmations confirmations,
        string? requestId = null) =>
        throw Violation(
            "CaptureProfileSnapshotRequired",
            "Hardware capture requires the exact approved readiness profile snapshot.");

    public static HardwareCameraAgentRequestEnvelope CreateCaptureRequest(
        string transactionId,
        string cameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        HardwareCaptureSafetyConfirmations confirmations,
        bool liveViewHandoffRequested,
        string? requestId = null)
    {
        ValidateCaptureProfileSnapshot(expectedProfile, requireFutureExpiry: true);
        ArgumentNullException.ThrowIfNull(confirmations);
        return CreateRequest(
            HardwareCameraAgentProtocol.Operations.CaptureSingle,
            new HardwareCaptureSingleRequest
            {
                TransactionId = transactionId,
                CameraAlias = cameraAlias,
                ExpectedCaptureProfileId = expectedProfile.ProfileId,
                ExpectedCaptureProfileVersion = expectedProfile.ProfileVersion,
                ExpectedCaptureProfileSha256 = expectedProfile.Sha256,
                ExpectedCaptureProfileExpiresAtUtc = FormatProfileUtc(expectedProfile.ExpiresAtUtc),
                ExclusiveCameraControlConfirmed = confirmations.ExclusiveCameraControlConfirmed,
                DedicatedSpoolScopeConfirmed = confirmations.DedicatedSpoolScopeConfirmed,
                ExactObjectDeleteConfirmed = confirmations.ExactObjectDeleteConfirmed,
                LiveViewHandoffRequested = liveViewHandoffRequested,
            },
            requestId);
    }

    public static HardwareCameraAgentRequestEnvelope CreateLiveViewProbeRequest(
        string cameraAlias,
        HardwareLiveViewSafetyConfirmation confirmation,
        int frames = 1,
        int intervalMs = 100,
        string? requestId = null)
    {
        ArgumentNullException.ThrowIfNull(confirmation);
        return CreateRequest(
            HardwareCameraAgentProtocol.Operations.LiveViewProbe,
            new HardwareLiveViewProbeRequest
            {
                CameraAlias = cameraAlias,
                ExclusiveCameraControlConfirmed = confirmation.ExclusiveCameraControlConfirmed,
                LiveViewFrames = frames,
                LiveViewIntervalMs = intervalMs,
            },
            requestId);
    }

    public static HardwareCameraAgentRequestEnvelope CreateTransactionResultRequest(
        string transactionId,
        string? requestId = null) =>
        CreateRequest(
            HardwareCameraAgentProtocol.Operations.GetTransactionResult,
            new HardwareTransactionResultRequest { TransactionId = transactionId },
            requestId);

    public static string SerializeRequest(HardwareCameraAgentRequestEnvelope envelope)
    {
        ValidateRequest(envelope);
        var json = JsonSerializer.Serialize(envelope, SerializerOptions);
        ValidateJsonLength(json);
        return json;
    }

    public static HardwareCameraAgentRequestEnvelope DeserializeRequest(string json)
    {
        var envelope = DeserializeEnvelope<HardwareCameraAgentRequestEnvelope>(json);
        ValidateRequest(envelope);
        return envelope;
    }

    public static HardwareCameraAgentReply<HardwareSingleReadinessResult> DeserializeReadinessResponse(
        string json,
        string expectedRequestId,
        string expectedCameraAlias)
    {
        ValidateAlias(expectedCameraAlias);
        var envelope = DeserializeResponseEnvelope(json, expectedRequestId);
        ThrowIfRemoteFailure(envelope);
        var payload = DeserializePayload<HardwareSingleReadinessResult>(envelope.Payload);
        ValidateReadinessResponse(envelope, payload, expectedCameraAlias);
        return ToReply(envelope, payload);
    }

    public static HardwareCameraAgentReply<HardwareSingleCaptureResult> DeserializeCaptureResponse(
        string json,
        string expectedRequestId,
        string expectedTransactionId,
        string expectedCameraAlias) =>
        DeserializeCaptureResponseCore(
            json,
            expectedRequestId,
            expectedTransactionId,
            expectedCameraAlias,
            expectedCaptureProfileSnapshot: null,
            expectedLiveViewHandoffRequested: false);

    public static HardwareCameraAgentReply<HardwareSingleCaptureResult> DeserializeCaptureResponse(
        string json,
        string expectedRequestId,
        string expectedTransactionId,
        string expectedCameraAlias,
        bool expectedLiveViewHandoffRequested)
        => DeserializeCaptureResponseCore(
            json,
            expectedRequestId,
            expectedTransactionId,
            expectedCameraAlias,
            expectedCaptureProfileSnapshot: null,
            expectedLiveViewHandoffRequested);

    public static HardwareCameraAgentReply<HardwareSingleCaptureResult> DeserializeCaptureResponse(
        string json,
        string expectedRequestId,
        string expectedTransactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot expectedCaptureProfileSnapshot,
        bool expectedLiveViewHandoffRequested)
    {
        ArgumentNullException.ThrowIfNull(expectedCaptureProfileSnapshot);
        return DeserializeCaptureResponseCore(
            json,
            expectedRequestId,
            expectedTransactionId,
            expectedCameraAlias,
            expectedCaptureProfileSnapshot,
            expectedLiveViewHandoffRequested);
    }

    private static HardwareCameraAgentReply<HardwareSingleCaptureResult> DeserializeCaptureResponseCore(
        string json,
        string expectedRequestId,
        string expectedTransactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot? expectedCaptureProfileSnapshot,
        bool expectedLiveViewHandoffRequested)
    {
        ValidateTransactionId(expectedTransactionId);
        ValidateAlias(expectedCameraAlias);
        if (expectedCaptureProfileSnapshot is not null)
        {
            ValidateCaptureProfileSnapshot(
                expectedCaptureProfileSnapshot,
                requireFutureExpiry: false);
        }
        var envelope = DeserializeResponseEnvelope(json, expectedRequestId);
        ThrowIfRemoteFailure(envelope);
        var payload = DeserializePayload<HardwareSingleCaptureResult>(envelope.Payload);
        ValidateCaptureResponse(
            envelope,
            payload,
            expectedTransactionId,
            expectedCameraAlias,
            isTransactionLookup: false,
            expectedCaptureProfileSnapshot,
            expectedLiveViewHandoffRequested);
        return ToReply(envelope, payload);
    }

    public static HardwareCameraAgentReply<HardwareSingleCaptureResult> DeserializeTransactionResultResponse(
        string json,
        string expectedRequestId,
        string expectedTransactionId)
        => DeserializeTransactionResultResponseCore(
            json,
            expectedRequestId,
            expectedTransactionId,
            expectedCameraAlias: null,
            expectedCaptureProfileSnapshot: null,
            expectedLiveViewHandoffRequested: null);

    public static HardwareCameraAgentReply<HardwareSingleCaptureResult> DeserializeTransactionResultResponse(
        string json,
        string expectedRequestId,
        string expectedTransactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot expectedCaptureProfileSnapshot,
        bool expectedLiveViewHandoffRequested)
    {
        ValidateAlias(expectedCameraAlias);
        ArgumentNullException.ThrowIfNull(expectedCaptureProfileSnapshot);
        return DeserializeTransactionResultResponseCore(
            json,
            expectedRequestId,
            expectedTransactionId,
            expectedCameraAlias,
            expectedCaptureProfileSnapshot,
            expectedLiveViewHandoffRequested);
    }

    private static HardwareCameraAgentReply<HardwareSingleCaptureResult> DeserializeTransactionResultResponseCore(
        string json,
        string expectedRequestId,
        string expectedTransactionId,
        string? expectedCameraAlias,
        HardwareCaptureProfileSnapshot? expectedCaptureProfileSnapshot,
        bool? expectedLiveViewHandoffRequested)
    {
        ValidateTransactionId(expectedTransactionId);
        if (expectedCaptureProfileSnapshot is not null)
        {
            ValidateCaptureProfileSnapshot(
                expectedCaptureProfileSnapshot,
                requireFutureExpiry: false);
        }

        var envelope = DeserializeResponseEnvelope(json, expectedRequestId);
        ThrowIfRemoteFailure(envelope);
        var payload = DeserializePayload<HardwareSingleCaptureResult>(envelope.Payload);
        ValidateCaptureResponse(
            envelope,
            payload,
            expectedTransactionId,
            expectedCameraAlias,
            isTransactionLookup: true,
            expectedCaptureProfileSnapshot,
            expectedLiveViewHandoffRequested);
        return ToReply(envelope, payload);
    }

    public static HardwareCameraAgentReply<HardwareSingleLiveViewResult> DeserializeLiveViewResponse(
        string json,
        string expectedRequestId,
        string expectedCameraAlias,
        int expectedFrames)
    {
        ValidateAlias(expectedCameraAlias);
        if (expectedFrames is < 1 or > 30)
        {
            throw Violation("InvalidLiveViewRequest", "The expected Live View frame count is invalid.");
        }

        var envelope = DeserializeResponseEnvelope(json, expectedRequestId);
        ThrowIfRemoteFailure(envelope);
        var payload = DeserializePayload<HardwareSingleLiveViewResult>(envelope.Payload);
        ValidateLiveViewResponse(envelope, payload, expectedCameraAlias, expectedFrames);
        return ToReply(envelope, payload);
    }

    private static HardwareCameraAgentRequestEnvelope CreateRequest<TPayload>(
        string operation,
        TPayload payload,
        string? requestId)
    {
        var envelope = new HardwareCameraAgentRequestEnvelope
        {
            SchemaVersion = HardwareCameraAgentProtocol.SchemaVersion,
            Simulation = false,
            Marker = HardwareCameraAgentProtocol.Marker,
            RequestId = requestId ?? Guid.NewGuid().ToString("N"),
            Operation = operation,
            Payload = JsonSerializer.SerializeToElement(payload, SerializerOptions),
        };

        ValidateRequest(envelope);
        return envelope;
    }

    private static void ValidateRequest(HardwareCameraAgentRequestEnvelope envelope)
    {
        ArgumentNullException.ThrowIfNull(envelope);
        ValidateCommon(envelope.SchemaVersion, envelope.Simulation, envelope.Marker, envelope.RequestId);
        if (!HardwareCameraAgentProtocol.Operations.IsSupported(envelope.Operation))
        {
            throw Violation("UnsupportedOperation", "The hardware operation is not supported.");
        }

        switch (envelope.Operation)
        {
            case HardwareCameraAgentProtocol.Operations.GetSingleReadiness:
                var readiness = DeserializePayload<HardwareReadinessRequest>(envelope.Payload);
                ValidateAlias(readiness.CameraAlias);
                break;
            case HardwareCameraAgentProtocol.Operations.CaptureSingle:
                var capture = DeserializePayload<HardwareCaptureSingleRequest>(envelope.Payload);
                ValidateTransactionId(capture.TransactionId);
                ValidateAlias(capture.CameraAlias);
                ValidateExpectedCaptureProfileFields(
                    capture.ExpectedCaptureProfileId,
                    capture.ExpectedCaptureProfileVersion,
                    capture.ExpectedCaptureProfileSha256,
                    capture.ExpectedCaptureProfileExpiresAtUtc);
                if (!capture.ExclusiveCameraControlConfirmed ||
                    !capture.DedicatedSpoolScopeConfirmed ||
                    !capture.ExactObjectDeleteConfirmed)
                {
                    throw Violation(
                        "SafetyConfirmationRequired",
                        "Capture requires every explicit hardware safety confirmation.");
                }

                break;
            case HardwareCameraAgentProtocol.Operations.LiveViewProbe:
                var liveView = DeserializePayload<HardwareLiveViewProbeRequest>(envelope.Payload);
                ValidateAlias(liveView.CameraAlias);
                if (!liveView.ExclusiveCameraControlConfirmed)
                {
                    throw Violation(
                        "SafetyConfirmationRequired",
                        "Live View requires exclusive camera-control confirmation.");
                }

                if (liveView.LiveViewFrames is < 1 or > 30 ||
                    liveView.LiveViewIntervalMs is < 0 or > 1000)
                {
                    throw Violation(
                        "InvalidLiveViewRequest",
                        "Live View bounds do not match the hardware protocol.");
                }

                break;
            case HardwareCameraAgentProtocol.Operations.GetTransactionResult:
                var transaction = DeserializePayload<HardwareTransactionResultRequest>(envelope.Payload);
                ValidateTransactionId(transaction.TransactionId);
                break;
        }
    }

    private static HardwareCameraAgentResponseEnvelope DeserializeResponseEnvelope(
        string json,
        string expectedRequestId)
    {
        ValidateRequestId(expectedRequestId);
        var envelope = DeserializeEnvelope<HardwareCameraAgentResponseEnvelope>(json);
        ValidateCommon(envelope.SchemaVersion, envelope.Simulation, envelope.Marker, envelope.RequestId);
        if (!string.Equals(envelope.RequestId, expectedRequestId, StringComparison.Ordinal))
        {
            throw Violation("RequestIdMismatch", "The response request ID does not match the request.");
        }

        if (!IsSafeToken(envelope.ResultCode, 128))
        {
            throw Violation("InvalidResultCode", "The response result code is invalid.");
        }

        ValidateObject(envelope.Payload);
        return envelope;
    }

    private static void ValidateReadinessResponse(
        HardwareCameraAgentResponseEnvelope envelope,
        HardwareSingleReadinessResult payload,
        string expectedCameraAlias)
    {
        ValidateCameraModeAndAlias(payload.CameraMode, payload.CameraAlias, expectedCameraAlias);
        if (!envelope.Success)
        {
            throw Violation("ForgedReadinessResult", "A typed readiness response must report a completed query.");
        }

        var expectedResultCode = payload.Ready ? "SingleReady" : "SingleNotReady";
        if (!string.Equals(envelope.ResultCode, expectedResultCode, StringComparison.Ordinal))
        {
            throw Violation("ForgedReadinessResult", "Readiness and resultCode disagree.");
        }

        if (payload.SdkCameraCount < 0 || payload.WpdCameraCount < 0 ||
            payload.SpoolPayloadObjectCount < 0)
        {
            throw Violation("InvalidReadinessResult", "Readiness counts cannot be negative.");
        }

        ValidateBoundedText(payload.Firmware, 128, "InvalidReadinessResult");
        ValidateBoundedText(payload.LiveViewStatus, 128, "InvalidReadinessResult");
        ValidateObservedSettings(payload.ObservedSettings);
        var profilePresent = ValidateCaptureProfileIdentity(
            payload.CaptureProfileId,
            payload.CaptureProfileVersion,
            payload.CaptureProfileSha256,
            payload.CaptureProfileCameraAlias,
            payload.CaptureProfileExpiresAtUtc,
            "InvalidReadinessResult");
        if (profilePresent != payload.CaptureProfileApproved ||
            payload.CaptureProfileAliasMatches !=
                (profilePresent && payload.CaptureProfileCameraAlias == payload.CameraAlias))
        {
            throw Violation(
                "InvalidReadinessResult",
                "Capture-profile identity, approval, and camera alias disagree.");
        }

        if (payload.SettingsMatchApprovedProfile &&
            (!payload.CaptureProfileApproved || !payload.CaptureProfileAliasMatches))
        {
            throw Violation(
                "InvalidReadinessResult",
                "Settings cannot match an unapproved or differently aliased capture profile.");
        }

        ValidateErrorFields(
            payload.FailureCategory,
            payload.FailureDetail,
            payload.Ready,
            payload.SpoolKnownEmpty);

        if (!payload.ReadOnly || payload.CaptureCommandSent ||
            payload.CameraObjectDeleteAttempted || payload.CameraSettingsChanged ||
            payload.RealIdentifiersIncluded)
        {
            throw Violation("UnsafeReadinessResult", "Readiness must remain read-only and redacted.");
        }

        if (payload.SpoolKnownEmpty && (!payload.SpoolInspected || payload.SpoolPayloadObjectCount != 0))
        {
            throw Violation("InvalidReadinessResult", "Spool-empty evidence is internally inconsistent.");
        }

        if (payload.Ready &&
            (payload.SdkCameraCount != 1 || payload.WpdCameraCount != 1 ||
             !payload.SdkIdentityBound || !payload.WpdIdentityBound ||
             !payload.SdkAliasMatches || !payload.WpdAliasMatches ||
             !payload.SdkStatusProbed || !payload.SpoolInspected ||
             !payload.SpoolKnownEmpty || payload.SpoolPayloadObjectCount != 0 ||
             !payload.LiveViewStatusAvailable ||
             !string.Equals(payload.LiveViewStatus, "off", StringComparison.Ordinal) ||
             !payload.CaptureProfileApproved || !payload.CaptureProfileAliasMatches ||
             !payload.SettingsMatchApprovedProfile ||
             payload.CaptureProfileExpiresAtUtc is null ||
             payload.CaptureProfileExpiresAtUtc.Value <= DateTimeOffset.UtcNow))
        {
            throw Violation(
                "ForgedReadinessResult",
                "SingleReady lacks required one-camera, Live View OFF, and approved-profile evidence.");
        }
    }

    private static void ValidateObservedSettings(HardwareObservedCameraSettings settings)
    {
        if (settings is null)
        {
            throw Violation("InvalidReadinessResult", "Observed camera settings are required.");
        }

        var values = new HardwareObservedCameraSetting?[]
        {
            settings.FileType,
            settings.CompressionLevel,
            settings.ImageSize,
            settings.ExposureMode,
            settings.ShutterSpeed,
            settings.Aperture,
            settings.Sensitivity,
            settings.WhiteBalanceMode,
            settings.FocusMode,
        };
        foreach (var setting in values)
        {
            if (setting is null)
            {
                throw Violation(
                    "InvalidReadinessResult",
                    "An observed camera setting has invalid capability metadata.");
            }

            ValidateBoundedText(setting.CapType, 128, "InvalidReadinessResult");
            ValidateBoundedText(setting.ProbeState, 128, "InvalidReadinessResult");
            ValidateBoundedText(setting.ValueType, 128, "InvalidReadinessResult");
            if (setting.CapType.Length == 0 || setting.ProbeState.Length == 0 ||
                setting.ValueType.Length == 0)
            {
                throw Violation(
                    "InvalidReadinessResult",
                    "An observed camera setting descriptor cannot be empty.");
            }

            if (setting.CurrentLabel is not null)
            {
                ValidateBoundedText(setting.CurrentLabel, 256, "InvalidReadinessResult");
                if (setting.CurrentLabel.Length == 0)
                {
                    throw Violation(
                        "InvalidReadinessResult",
                        "An observed camera setting label cannot be empty.");
                }
            }

            var hasCurrentValue = setting.CurrentValue.HasValue ||
                setting.CurrentIndex.HasValue || setting.CurrentLabel is not null;
            if ((setting.Available && !hasCurrentValue) || (!setting.Available && hasCurrentValue))
            {
                throw Violation(
                    "InvalidReadinessResult",
                    "Observed camera setting availability and current value disagree.");
            }
        }
    }

    private static void ValidateCaptureResponse(
        HardwareCameraAgentResponseEnvelope envelope,
        HardwareSingleCaptureResult payload,
        string expectedTransactionId,
        string? expectedCameraAlias,
        bool isTransactionLookup,
        HardwareCaptureProfileSnapshot? expectedCaptureProfileSnapshot,
        bool? expectedLiveViewHandoffRequested)
    {
        if (!string.Equals(payload.CameraMode, "SingleCamera", StringComparison.Ordinal))
        {
            throw Violation("UnsupportedCameraMode", "Hardware v1 accepts SingleCamera responses only.");
        }

        if (!string.Equals(payload.TransactionId, expectedTransactionId, StringComparison.Ordinal) ||
            !IsValidTransactionId(payload.TransactionId))
        {
            throw Violation("TransactionIdMismatch", "The response transaction ID does not match the request.");
        }

        var missingRecord = isTransactionLookup && envelope.ResultCode is
            "TransactionNotFound" or "transaction_reservation_incomplete";
        if (!missingRecord && isTransactionLookup && envelope.ResultCode == "TransactionReserved" &&
            string.IsNullOrEmpty(payload.CameraAlias) &&
            string.IsNullOrEmpty(payload.RequiredCameraAlias) &&
            string.IsNullOrEmpty(payload.RunId))
        {
            // TransactionReserved is reported for two distinct wire shapes from the
            // Camera Agent: a lease-busy race observed before the transaction journal
            // exists (no durable record yet, alias/runId empty), and a lease-busy race
            // observed after the journal is committed (durable record with its assigned
            // alias/runId already present). Only the former is a "missing record" -
            // treating every TransactionReserved response as missingRecord would reject
            // the populated-alias shape below via the empty-field check.
            missingRecord = true;
        }

        if (missingRecord)
        {
            if (!string.IsNullOrEmpty(payload.CameraAlias) ||
                !string.IsNullOrEmpty(payload.RequiredCameraAlias) ||
                !string.IsNullOrEmpty(payload.RunId))
            {
                throw Violation("InvalidTransactionResult", "A missing or invalid journal cannot assert a camera or run.");
            }
        }
        else
        {
            ValidateAlias(payload.CameraAlias);
            ValidateAlias(payload.RequiredCameraAlias);
            if (payload.RequiredCameraAlias != payload.CameraAlias)
            {
                throw Violation(
                    "RequiredCameraAliasMismatch",
                    "The durable required camera alias does not match the capture alias.");
            }

            ValidateRunId(payload.RunId);
        }

        if (!missingRecord && expectedCameraAlias is not null &&
            !string.Equals(payload.CameraAlias, expectedCameraAlias, StringComparison.Ordinal))
        {
            throw Violation("CameraAliasMismatch", "The capture response alias does not match the request.");
        }

        if (!missingRecord && expectedLiveViewHandoffRequested.HasValue &&
            payload.LiveViewHandoffRequested != expectedLiveViewHandoffRequested.Value)
        {
            throw Violation(
                "LiveViewHandoffMismatch",
                "The capture response does not match the requested Live View handoff mode.");
        }

        if (payload.TerminalState is not ("Reserved" or "InProgress" or "Complete" or "Blocked" or "FailedPartial"))
        {
            throw Violation("InvalidTerminalState", "The capture terminal state is not supported.");
        }

        ValidateBoundedText(payload.ErrorCategory, 128, "InvalidCaptureResult");
        ValidateBoundedText(payload.ErrorDetail, MaximumErrorDetailLength, "InvalidCaptureResult");
        var captureProfilePresent = ValidateCaptureProfileIdentity(
            payload.CaptureProfileId,
            payload.CaptureProfileVersion,
            payload.CaptureProfileSha256,
            payload.CaptureProfileCameraAlias,
            payload.CaptureProfileExpiresAtUtc,
            "InvalidCaptureResult");
        ValidateCaptureProfileCorrelation(
            envelope,
            payload,
            expectedCaptureProfileSnapshot,
            captureProfilePresent,
            missingRecord);
        var reportsProfilePreflightBlock = !envelope.Success &&
            payload.TerminalState == "Blocked" &&
            envelope.ResultCode == payload.ErrorCategory &&
            payload.ErrorCategory is
                "capture_profile_not_approved" or
                "capture_profile_expired" or
                "capture_profile_alias_mismatch" or
                "capture_profile_snapshot_mismatch";
        if (!missingRecord && expectedCameraAlias is not null && captureProfilePresent &&
            !reportsProfilePreflightBlock &&
            !string.Equals(
                payload.CaptureProfileCameraAlias,
                expectedCameraAlias,
                StringComparison.Ordinal))
        {
            throw Violation(
                "CaptureProfileCameraAliasMismatch",
                "The capture-profile alias does not match the durable required camera alias.");
        }
        if (payload.AutomaticRetryCount != 0 || payload.TransactionWatchdogSeconds != 180 ||
            payload.RealIdentifiersIncluded)
        {
            throw Violation("UnsafeCaptureResult", "Capture retry, watchdog, or redaction guarantees were violated.");
        }

        if (payload.RetainedOriginal is not null)
        {
            ValidateAlias(payload.RetainedOriginal.CameraAlias);
            if (!string.Equals(payload.RetainedOriginal.CameraAlias, payload.CameraAlias, StringComparison.Ordinal))
            {
                throw Violation("CameraAliasMismatch", "The retained original alias does not match its capture.");
            }

            ValidateVerifiedJpeg(
                payload.RetainedOriginal.Path,
                payload.RetainedOriginal.SizeBytes,
                payload.RetainedOriginal.Sha256,
                "original.jpg");
        }


        ValidateCaptureLiveViewHandoff(payload);

        if (missingRecord && (captureProfilePresent || payload.LiveViewHandoffRequested))
        {
            throw Violation(
                "InvalidTransactionResult",
                "A missing or incomplete journal cannot assert capture-profile or handoff state.");
        }

        if (payload.CameraObjectDeleteSucceeded &&
            (!payload.CameraObjectDeleteAttempted || payload.RetainedOriginal is null ||
             !payload.SpoolEmptyBeforeCapture))
        {
            throw Violation("UnsafeCaptureResult", "Camera-object deletion lacks its required persistence evidence.");
        }

        if (envelope.Success)
        {
            if (!string.Equals(envelope.ResultCode, "CaptureComplete", StringComparison.Ordinal) ||
                payload.TerminalState != "Complete" || payload.RetainedOriginal is null ||
                !captureProfilePresent ||
                payload.CaptureProfileCameraAlias != payload.CameraAlias ||
                payload.CaptureProfileExpiresAtUtc is null ||
                !payload.SpoolEmptyBeforeCapture || !payload.CameraObjectDeleteAttempted ||
                !payload.CameraObjectDeleteSucceeded || !payload.SpoolEmptyAfterCleanup ||
                (payload.LiveViewHandoffRequested &&
                 (!payload.LiveViewStoppedBeforeCapture ||
                  !payload.LiveViewSdkSessionClosedBeforeCapture ||
                  !payload.PostCaptureLiveViewProbeAttempted ||
                  !payload.PostCaptureLiveViewProbeSucceeded ||
                  payload.PostCapturePreview is null)) ||
                !string.IsNullOrEmpty(payload.ErrorCategory) || !string.IsNullOrEmpty(payload.ErrorDetail))
            {
                throw Violation("ForgedCaptureSuccess", "Capture success lacks verified terminal evidence.");
            }

            return;
        }

        if (envelope.ResultCode == "CaptureComplete" || payload.TerminalState == "Complete")
        {
            throw Violation("ForgedCaptureSuccess", "A failed response cannot claim capture completion.");
        }

        if (!isTransactionLookup && payload.TerminalState is ("Reserved" or "InProgress"))
        {
            throw Violation("InvalidCaptureResult", "capture-single cannot return an active transaction result.");
        }

        switch (envelope.ResultCode)
        {
            case "TransactionReserved" when isTransactionLookup:
                if (payload.TerminalState != "Reserved" ||
                    (payload.ErrorCategory.Length != 0 &&
                     payload.ErrorCategory != "transaction_reserved") ||
                    payload.RetainedOriginal is not null || payload.SpoolEmptyBeforeCapture ||
                    payload.CameraObjectDeleteAttempted || payload.CameraObjectDeleteSucceeded ||
                    payload.SpoolEmptyAfterCleanup)
                {
                    throw Violation("InvalidTransactionResult", "Reserved lookup evidence is inconsistent.");
                }

                break;
            case "TransactionInProgress" when isTransactionLookup:
                if (payload.TerminalState != "InProgress" ||
                    (payload.ErrorCategory.Length != 0 &&
                     payload.ErrorCategory != "transaction_in_progress") ||
                    payload.RetainedOriginal is not null || payload.SpoolEmptyBeforeCapture ||
                    payload.CameraObjectDeleteAttempted || payload.CameraObjectDeleteSucceeded ||
                    payload.SpoolEmptyAfterCleanup)
                {
                    throw Violation("InvalidTransactionResult", "In-progress lookup evidence is inconsistent.");
                }

                break;
            case "transaction_reservation_incomplete" when isTransactionLookup:
                if (payload.TerminalState != "FailedPartial" ||
                    payload.ErrorCategory != "transaction_reservation_incomplete" ||
                    payload.RetainedOriginal is not null || payload.SpoolEmptyBeforeCapture ||
                    payload.CameraObjectDeleteAttempted || payload.CameraObjectDeleteSucceeded ||
                    payload.SpoolEmptyAfterCleanup)
                {
                    throw Violation("InvalidTransactionResult", "Incomplete reservation evidence is inconsistent.");
                }

                break;
            case "TransactionNotFound" when isTransactionLookup:
                if (payload.TerminalState != "Blocked" ||
                    payload.ErrorCategory != "transaction_not_found" ||
                    payload.RetainedOriginal is not null || payload.SpoolEmptyBeforeCapture ||
                    payload.CameraObjectDeleteAttempted || payload.CameraObjectDeleteSucceeded ||
                    payload.SpoolEmptyAfterCleanup)
                {
                    throw Violation("InvalidTransactionResult", "Not-found lookup evidence is inconsistent.");
                }

                break;
            case "CaptureFailed":
                if (!string.IsNullOrEmpty(payload.ErrorCategory))
                {
                    throw Violation("InvalidCaptureResult", "CaptureFailed is reserved for an empty error category.");
                }

                break;
            default:
                if (!string.Equals(envelope.ResultCode, payload.ErrorCategory, StringComparison.Ordinal))
                {
                    throw Violation("InvalidCaptureResult", "Capture failure and resultCode disagree.");
                }

                break;
        }
    }

    private static bool ValidateCaptureProfileIdentity(
        string profileId,
        uint profileVersion,
        string profileSha256,
        string profileCameraAlias,
        DateTimeOffset? profileExpiresAtUtc,
        string errorCode)
    {
        var profilePresent = !string.IsNullOrEmpty(profileId);
        if (!profilePresent)
        {
            if (profileVersion != 0 || !string.IsNullOrEmpty(profileSha256) ||
                !string.IsNullOrEmpty(profileCameraAlias) || profileExpiresAtUtc.HasValue)
            {
                throw Violation(errorCode, "Capture-profile identity is only partially populated.");
            }

            return false;
        }

        if (!IsSafeToken(profileId, 128) || profileVersion == 0 ||
            !IsLowerHex(profileSha256, 64) ||
            profileCameraAlias is not ("CAM-A" or "CAM-B") ||
            !profileExpiresAtUtc.HasValue ||
            profileExpiresAtUtc.Value.Offset != TimeSpan.Zero)
        {
            throw Violation(errorCode, "Capture-profile identity is invalid.");
        }

        return true;
    }

    private static void ValidateCaptureProfileCorrelation(
        HardwareCameraAgentResponseEnvelope envelope,
        HardwareSingleCaptureResult payload,
        HardwareCaptureProfileSnapshot? expectedProfile,
        bool profilePresent,
        bool missingRecord)
    {
        if (expectedProfile is null || missingRecord)
        {
            return;
        }

        var identityHashMatches = profilePresent &&
            payload.CaptureProfileId == expectedProfile.ProfileId &&
            payload.CaptureProfileVersion == expectedProfile.ProfileVersion &&
            payload.CaptureProfileSha256 == expectedProfile.Sha256;
        if (identityHashMatches &&
            payload.CaptureProfileExpiresAtUtc != expectedProfile.ExpiresAtUtc)
        {
            throw Violation(
                "CaptureProfileSnapshotMismatch",
                "An identical capture-profile SHA cannot assert a different expiry.");
        }

        var matchesExpected = identityHashMatches &&
            payload.CaptureProfileExpiresAtUtc == expectedProfile.ExpiresAtUtc;
        var reportsProfilePreflightBlock = !envelope.Success &&
            payload.TerminalState == "Blocked" &&
            envelope.ResultCode == payload.ErrorCategory &&
            payload.ErrorCategory is
                "capture_profile_not_approved" or
                "capture_profile_expired" or
                "capture_profile_alias_mismatch" or
                "capture_profile_snapshot_mismatch";
        if (reportsProfilePreflightBlock)
        {
            if (payload.ErrorCategory == "capture_profile_snapshot_mismatch" && matchesExpected)
            {
                throw Violation(
                    "ForgedCaptureProfileMismatch",
                    "The agent reported a profile mismatch for an identical profile snapshot.");
            }

            return;
        }

        if (!matchesExpected)
        {
            throw Violation(
                "CaptureProfileSnapshotMismatch",
                "The capture response profile does not match the readiness snapshot.");
        }
    }

    private static void ValidateCaptureLiveViewHandoff(HardwareSingleCaptureResult payload)
    {
        if (!payload.LiveViewHandoffRequested &&
            (payload.LiveViewStoppedBeforeCapture ||
             payload.LiveViewSdkSessionClosedBeforeCapture ||
             payload.PostCaptureLiveViewProbeAttempted ||
             payload.PostCaptureLiveViewProbeSucceeded ||
             payload.PostCapturePreview is not null))
        {
            throw Violation(
                "InvalidLiveViewHandoff",
                "A capture without Live View handoff cannot assert handoff evidence.");
        }

        if (payload.LiveViewStoppedBeforeCapture !=
            payload.LiveViewSdkSessionClosedBeforeCapture)
        {
            throw Violation(
                "InvalidLiveViewHandoff",
                "Live View stop and SDK-session close evidence must agree.");
        }

        if (payload.PostCaptureLiveViewProbeAttempted &&
            (!payload.LiveViewHandoffRequested ||
             !payload.LiveViewStoppedBeforeCapture ||
             !payload.LiveViewSdkSessionClosedBeforeCapture))
        {
            throw Violation(
                "InvalidLiveViewHandoff",
                "Live View resume cannot precede a verified stop and SDK close.");
        }

        if (payload.PostCaptureLiveViewProbeSucceeded &&
            (!payload.PostCaptureLiveViewProbeAttempted || payload.PostCapturePreview is null))
        {
            throw Violation(
                "InvalidLiveViewHandoff",
                "A resumed Live View requires an attempted resume and verified preview.");
        }

        if (payload.PostCapturePreview is not null)
        {
            if (!payload.PostCaptureLiveViewProbeSucceeded)
            {
                throw Violation(
                    "InvalidLiveViewHandoff",
                    "A resumed preview cannot exist when Live View was not resumed.");
            }

            ValidateVerifiedJpeg(
                payload.PostCapturePreview.Path,
                payload.PostCapturePreview.SizeBytes,
                payload.PostCapturePreview.Sha256,
                "preview.jpg");
        }
    }

    private static void ValidateLiveViewResponse(
        HardwareCameraAgentResponseEnvelope envelope,
        HardwareSingleLiveViewResult payload,
        string expectedCameraAlias,
        int expectedFrames)
    {
        ValidateCameraModeAndAlias(payload.CameraMode, payload.CameraAlias, expectedCameraAlias);
        ValidateRunId(payload.RunId);
        if (payload.PreviewIsOriginal || payload.PreviewIsStitchInput || payload.RealIdentifiersIncluded)
        {
            throw Violation("UnsafeLiveViewResult", "Live View preview provenance or redaction is invalid.");
        }

        ValidateBoundedText(payload.ErrorCategory, 128, "InvalidLiveViewResult");
        ValidateBoundedText(payload.ErrorDetail, MaximumErrorDetailLength, "InvalidLiveViewResult");
        if (payload.Frames is < 0 or > 30 || payload.LastFrameBytes < 0 || payload.DurationMs < 0)
        {
            throw Violation("InvalidLiveViewResult", "Live View metrics are outside their allowed range.");
        }

        if (payload.Preview is not null)
        {
            ValidateVerifiedJpeg(
                payload.Preview.Path,
                payload.Preview.SizeBytes,
                payload.Preview.Sha256,
                "preview.jpg");
        }

        if (envelope.Success)
        {
            if (envelope.ResultCode != "LiveViewProbeComplete" || payload.Frames != expectedFrames ||
                payload.LastFrameBytes <= 0 || !IsLowerHex(payload.LastFrameSha256, 64) ||
                !payload.PreviewPersisted || payload.Preview is null ||
                payload.Preview.SizeBytes != payload.LastFrameBytes ||
                payload.Preview.Sha256 != payload.LastFrameSha256 ||
                !payload.LiveViewStopped || !payload.SdkSessionClosed ||
                !string.IsNullOrEmpty(payload.ErrorCategory) || !string.IsNullOrEmpty(payload.ErrorDetail))
            {
                throw Violation("ForgedLiveViewSuccess", "Live View success lacks verified terminal preview evidence.");
            }

            return;
        }

        if (envelope.ResultCode == "LiveViewProbeComplete" || payload.PreviewPersisted ||
            payload.Preview is not null)
        {
            throw Violation("ForgedLiveViewSuccess", "A failed Live View response claims a persisted preview.");
        }

        var hasObservedFrame = payload.Frames != 0 || payload.LastFrameBytes != 0 ||
            !string.IsNullOrEmpty(payload.LastFrameSha256);
        if (hasObservedFrame)
        {
            if (payload.Frames != expectedFrames || payload.LastFrameBytes <= 0 ||
                !IsLowerHex(payload.LastFrameSha256, 64) ||
                !payload.LiveViewStopped || !payload.SdkSessionClosed)
            {
                throw Violation(
                    "InvalidLiveViewResult",
                    "Failed Live View frame evidence is internally inconsistent.");
            }
        }
        else if (payload.LiveViewStopped || payload.SdkSessionClosed)
        {
            throw Violation("InvalidLiveViewResult", "Failed Live View cleanup evidence is inconsistent.");
        }

        if (envelope.ResultCode == "LiveViewProbeFailed")
        {
            if (!string.IsNullOrEmpty(payload.ErrorCategory))
            {
                throw Violation("InvalidLiveViewResult", "LiveViewProbeFailed requires an empty error category.");
            }
        }
        else if (!string.Equals(envelope.ResultCode, payload.ErrorCategory, StringComparison.Ordinal))
        {
            throw Violation("InvalidLiveViewResult", "Live View failure and resultCode disagree.");
        }
    }

    private static void ThrowIfRemoteFailure(HardwareCameraAgentResponseEnvelope envelope)
    {
        if (!envelope.Payload.TryGetProperty("cameraAccess", out _))
        {
            if (envelope.ResultCode == "ProtocolRejected")
            {
                throw Violation(
                    "ForgedAgentFailure",
                    "ProtocolRejected requires the exact hardware rejection payload.");
            }

            return;
        }

        var payload = DeserializePayload<HardwareCameraAgentFailure>(envelope.Payload);
        if (envelope.Success || payload.CameraAccess != "None" || payload.RealIdentifiersIncluded)
        {
            throw Violation("ForgedAgentFailure", "The hardware failure envelope is internally inconsistent.");
        }

        ValidateBoundedText(payload.ErrorDetail, MaximumErrorDetailLength, "InvalidAgentFailure");
        if (envelope.ResultCode == "ProtocolRejected")
        {
            if (!IsSafeToken(payload.RejectionCode, 128))
            {
                throw Violation("InvalidAgentFailure", "A protocol rejection code is required.");
            }
        }
        else if (!string.IsNullOrEmpty(payload.RejectionCode))
        {
            throw Violation("InvalidAgentFailure", "Agent failures outside protocol rejection cannot assert a rejection code.");
        }

        throw new HardwareCameraAgentRemoteException(
            envelope.RequestId,
            envelope.ResultCode,
            payload.RejectionCode,
            payload.ErrorDetail);
    }

    private static HardwareCameraAgentReply<T> ToReply<T>(
        HardwareCameraAgentResponseEnvelope envelope,
        T payload) =>
        new(envelope.RequestId, envelope.Success, envelope.ResultCode, payload);

    private static T DeserializeEnvelope<T>(string json)
    {
        ValidateJsonLength(json);
        ValidateNoDuplicateProperties(json);
        try
        {
            return JsonSerializer.Deserialize<T>(json, SerializerOptions)
                ?? throw Violation("MalformedEnvelope", "The hardware envelope was null.");
        }
        catch (HardwareProtocolViolationException)
        {
            throw;
        }
        catch (JsonException exception)
        {
            throw Violation("MalformedEnvelope", "Hardware protocol JSON is malformed.", exception);
        }
    }

    private static T DeserializePayload<T>(JsonElement payload)
    {
        ValidateObject(payload);
        var json = payload.GetRawText();
        ValidateNoDuplicateProperties(json);
        try
        {
            return JsonSerializer.Deserialize<T>(json, SerializerOptions)
                ?? throw Violation("InvalidPayload", "The hardware payload was null.");
        }
        catch (HardwareProtocolViolationException)
        {
            throw;
        }
        catch (JsonException exception)
        {
            throw Violation("InvalidPayload", "The hardware payload fields are invalid.", exception);
        }
    }

    private static void ValidateCommon(
        string schemaVersion,
        bool simulation,
        string marker,
        string requestId)
    {
        if (!string.Equals(schemaVersion, HardwareCameraAgentProtocol.SchemaVersion, StringComparison.Ordinal))
        {
            throw Violation("UnsupportedSchemaVersion", "The hardware schema version is not supported.");
        }

        if (simulation)
        {
            throw Violation("HardwareProtocolRequired", "Hardware v1 requires simulation=false.");
        }

        if (!string.Equals(marker, HardwareCameraAgentProtocol.Marker, StringComparison.Ordinal))
        {
            throw Violation("HardwareMarkerRequired", "The Hardware marker is required.");
        }

        ValidateRequestId(requestId);
    }

    private static void ValidateRequestId(string requestId)
    {
        if (!IsSafeToken(requestId, 128))
        {
            throw Violation("InvalidRequestId", "A bounded safe request ID is required.");
        }
    }

    private static void ValidateTransactionId(string transactionId)
    {
        if (!IsValidTransactionId(transactionId))
        {
            throw Violation("InvalidTransactionId", "A transaction ID must contain exactly 32 hexadecimal characters.");
        }
    }

    private static void ValidateCaptureProfileSnapshot(
        HardwareCaptureProfileSnapshot snapshot,
        bool requireFutureExpiry)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        ValidateExpectedCaptureProfileFields(
            snapshot.ProfileId,
            snapshot.ProfileVersion,
            snapshot.Sha256);
        if (snapshot.ExpiresAtUtc.Offset != TimeSpan.Zero ||
            snapshot.ExpiresAtUtc.Ticks % TimeSpan.TicksPerSecond != 0 ||
            (requireFutureExpiry && snapshot.ExpiresAtUtc <= DateTimeOffset.UtcNow))
        {
            throw Violation(
                "InvalidCaptureProfileSnapshot",
                "The capture-profile snapshot expiry must be future UTC.");
        }
    }

    private static void ValidateExpectedCaptureProfileFields(
        string profileId,
        uint profileVersion,
        string profileSha256)
    {
        if (!IsSafeToken(profileId, 128) || profileVersion == 0 ||
            !IsLowerHex(profileSha256, 64))
        {
            throw Violation(
                "InvalidCaptureProfileSnapshot",
                "Capture requires a valid readiness-approved profile ID, version, and SHA-256.");
        }
    }

    private static void ValidateExpectedCaptureProfileFields(
        string profileId,
        uint profileVersion,
        string profileSha256,
        string profileExpiresAtUtc)
    {
        ValidateExpectedCaptureProfileFields(profileId, profileVersion, profileSha256);
        if (!DateTimeOffset.TryParseExact(
                profileExpiresAtUtc,
                ProfileUtcWireFormat,
                CultureInfo.InvariantCulture,
                DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal,
                out var expiry) || expiry <= DateTimeOffset.UtcNow)
        {
            throw Violation(
                "InvalidCaptureProfileSnapshot",
                "Capture requires a future second-precision UTC profile expiry.");
        }
    }

    private static string FormatProfileUtc(DateTimeOffset value) =>
        value.ToUniversalTime().ToString(ProfileUtcWireFormat, CultureInfo.InvariantCulture);

    private static void ValidateAlias(string cameraAlias)
    {
        if (cameraAlias is not ("CAM-A" or "CAM-B"))
        {
            throw Violation("InvalidAlias", "The hardware camera alias must be CAM-A or CAM-B.");
        }
    }

    private static void ValidateCameraModeAndAlias(
        string cameraMode,
        string cameraAlias,
        string expectedCameraAlias)
    {
        if (!string.Equals(cameraMode, "SingleCamera", StringComparison.Ordinal))
        {
            throw Violation("UnsupportedCameraMode", "Hardware v1 accepts SingleCamera responses only.");
        }

        ValidateAlias(cameraAlias);
        if (!string.Equals(cameraAlias, expectedCameraAlias, StringComparison.Ordinal))
        {
            throw Violation("CameraAliasMismatch", "The response alias does not match the request.");
        }
    }

    private static void ValidateRunId(string runId)
    {
        if (string.IsNullOrEmpty(runId) || runId.Length is < 7 or > 64 ||
            !runId.StartsWith("run-", StringComparison.Ordinal))
        {
            throw Violation("InvalidRunId", "The hardware run ID is invalid.");
        }

        var separator = runId.IndexOf('-', 4);
        if (separator <= 4 || separator == runId.Length - 1 ||
            !runId.AsSpan(4, separator - 4).ToArray().All(IsAsciiDigit) ||
            !runId.AsSpan(separator + 1).ToArray().All(IsAsciiDigit))
        {
            throw Violation("InvalidRunId", "The hardware run ID is invalid.");
        }
    }

    private static void ValidateVerifiedJpeg(
        string path,
        long sizeBytes,
        string sha256,
        string requiredFileName)
    {
        if (sizeBytes is <= 0 or > MaximumVerifiedJpegBytes || !IsLowerHex(sha256, 64))
        {
            throw Violation("InvalidVerifiedJpeg", "Verified JPEG size or SHA-256 is invalid.");
        }

        ValidateAbsoluteLocalWindowsPath(path, requiredFileName);
    }

    private static void ValidateAbsoluteLocalWindowsPath(string path, string requiredFileName)
    {
        if (string.IsNullOrEmpty(path) || path.Length > 1024 || path.Length < 4 ||
            !IsAsciiLetter(path[0]) || path[1] != ':' || !IsDirectorySeparator(path[2]) ||
            path.StartsWith("\\\\", StringComparison.Ordinal) ||
            path.StartsWith("\\\\?\\", StringComparison.Ordinal) ||
            path.Any(character => character < 0x20 || character is '<' or '>' or '"' or '|' or '?' or '*'))
        {
            throw Violation("InvalidArtifactPath", "The artifact path must be an absolute local Windows path.");
        }

        var components = path[3..].Split(['\\', '/'], StringSplitOptions.None);
        if (components.Length == 0 || components.Any(component =>
                string.IsNullOrEmpty(component) || component is "." or ".." ||
                component.Contains(':', StringComparison.Ordinal) ||
                component.EndsWith(' ') || component.EndsWith('.')) ||
            !string.Equals(components[^1], requiredFileName, StringComparison.OrdinalIgnoreCase))
        {
            throw Violation("InvalidArtifactPath", "The artifact path is not canonical for this result.");
        }

    }

    private static void ValidateErrorFields(
        string category,
        string detail,
        bool success,
        bool spoolKnownEmpty)
    {
        ValidateBoundedText(category, 128, "InvalidReadinessResult");
        ValidateBoundedText(detail, MaximumErrorDetailLength, "InvalidReadinessResult");
        if (success && (!string.IsNullOrEmpty(category) || !string.IsNullOrEmpty(detail)))
        {
            throw Violation("ForgedReadinessResult", "A ready response cannot include failure details.");
        }

        if (!success && string.IsNullOrEmpty(category) && spoolKnownEmpty)
        {
            throw Violation("InvalidReadinessResult", "A not-ready response lacks a blocking condition.");
        }
    }

    private static void ValidateBoundedText(string value, int maximumLength, string errorCode)
    {
        if (value is null || value.Length > maximumLength || value.Any(character => character < 0x20))
        {
            throw Violation(errorCode, "A hardware response string is invalid or unbounded.");
        }
    }

    private static void ValidateObject(JsonElement payload)
    {
        if (payload.ValueKind != JsonValueKind.Object)
        {
            throw Violation("InvalidPayload", "The hardware payload must be a JSON object.");
        }
    }

    private static void ValidateJsonLength(string json)
    {
        if (string.IsNullOrEmpty(json) || Encoding.UTF8.GetByteCount(json) > MaximumProtocolJsonBytes)
        {
            throw Violation("MalformedEnvelope", "Hardware protocol JSON length is outside the allowed range.");
        }
    }

    private static void ValidateNoDuplicateProperties(string json)
    {
        try
        {
            using var document = JsonDocument.Parse(
                json,
                new JsonDocumentOptions
                {
                    AllowTrailingCommas = false,
                    CommentHandling = JsonCommentHandling.Disallow,
                    MaxDepth = 8,
                });
            ValidateNoDuplicateProperties(document.RootElement);
        }
        catch (HardwareProtocolViolationException)
        {
            throw;
        }
        catch (JsonException exception)
        {
            throw Violation("MalformedEnvelope", "Hardware protocol JSON is malformed.", exception);
        }
    }

    private static void ValidateNoDuplicateProperties(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in element.EnumerateObject())
            {
                if (!names.Add(property.Name))
                {
                    throw Violation("DuplicateField", "Duplicate hardware protocol JSON fields are rejected.");
                }

                ValidateNoDuplicateProperties(property.Value);
            }
        }
        else if (element.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in element.EnumerateArray())
            {
                ValidateNoDuplicateProperties(item);
            }
        }
    }

    private static bool IsValidTransactionId(string? value) =>
        value is { Length: 32 } && value.All(IsAsciiHex);

    private static bool IsLowerHex(string? value, int length) =>
        value is not null && value.Length == length && value.All(character =>
            IsAsciiDigit(character) || character is >= 'a' and <= 'f');

    private static bool IsSafeToken(string? value, int maximumLength) =>
        !string.IsNullOrEmpty(value) && value.Length <= maximumLength && value.All(character =>
            IsAsciiLetter(character) || IsAsciiDigit(character) || character is '.' or '-' or '_');

    private static bool IsAsciiHex(char character) =>
        IsAsciiDigit(character) || character is >= 'a' and <= 'f' or >= 'A' and <= 'F';

    private static bool IsAsciiDigit(char character) => character is >= '0' and <= '9';

    private static bool IsAsciiLetter(char character) =>
        character is >= 'a' and <= 'z' or >= 'A' and <= 'Z';

    private static bool IsDirectorySeparator(char character) => character is '\\' or '/';

    private static HardwareProtocolViolationException Violation(
        string errorCode,
        string message,
        Exception? innerException = null) =>
        new(errorCode, message, innerException);
}
