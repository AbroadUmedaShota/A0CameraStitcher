using System.Collections.ObjectModel;
using System.Text.Json.Serialization;

namespace A0CameraStitcher.M3.Foundation.DualCamera;

[JsonConverter(typeof(JsonStringEnumConverter<DualCameraExecutionEnvironment>))]
public enum DualCameraExecutionEnvironment
{
    TestSynthetic,
    HardwareDual,
}

public enum DualCameraProductStage
{
    CaptureCameraA,
    ValidateCameraA,
    CaptureCameraB,
    ValidateCameraB,
    Stitch,
    Review,
    Export,
}

public enum DualCameraStageStatus
{
    Pending,
    Active,
    Succeeded,
    Failed,
}

public enum DualCameraFailureCode
{
    None,
    InvalidMode,
    InvalidExecutionEnvironment,
    IdentityNotReady,
    InvalidProfile,
    DuplicateStart,
    CaptureCameraA,
    CaptureCameraB,
    InvalidOriginal,
    LiveViewStopFailed,
    HardwarePending,
    ProfileMismatch,
    SpoolNotEmpty,
    WatchdogExpired,
    AgentResponseUnknown,
    Interrupted,
    StitchFailed,
    ExportFailed,
}

public enum DualCameraProfileStatus
{
    Draft,
    Approved,
}

public enum DualCameraTestFault
{
    None,
    FailBeforeCapture,
    FailCaptureCameraA,
    FailCaptureCameraB,
    InterruptAfterCameraA,
    FailStitch,
}

public sealed record DualCameraRigProfile
{
    public required string ProfileId { get; init; }

    public required string Version { get; init; }

    public required DualCameraProfileStatus Status { get; init; }

    public required string SchemaVersion { get; init; }

    public required string Provenance { get; init; }

    public required DateTimeOffset MeasuredAtUtc { get; init; }

    public required DateTimeOffset ValidUntilUtc { get; init; }

    public required DateTimeOffset AssessedAtUtc { get; init; }

    public required int ExpectedInputWidth { get; init; }

    public required int ExpectedInputHeight { get; init; }

    public required IReadOnlyList<double> CameraBToCameraA { get; init; }

    public required string Layout { get; init; }

    public required IReadOnlyList<int> Crop { get; init; }

    public IReadOnlyList<string> CameraAliases { get; init; } = Array.AsReadOnly(["CAM-A", "CAM-B"]);

    public static DualCameraRigProfile ApprovedSynthetic() => new()
    {
        ProfileId = "synthetic-approved-rig-v1",
        Version = "1",
        Status = DualCameraProfileStatus.Approved,
        SchemaVersion = "1.1.0",
        Provenance = "anonymous-generated-test-fixture",
        MeasuredAtUtc = DateTimeOffset.FromUnixTimeSeconds(100),
        ValidUntilUtc = DateTimeOffset.FromUnixTimeSeconds(4_102_444_800),
        AssessedAtUtc = DateTimeOffset.FromUnixTimeSeconds(200),
        ExpectedInputWidth = 16,
        ExpectedInputHeight = 8,
        CameraBToCameraA = Array.AsReadOnly([1.0, 0.0, 12.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]),
        Layout = "camera-a-left-camera-b-right",
        Crop = Array.AsReadOnly([1, 1, 1, 1]),
    };

    public void Validate(DateTimeOffset? nowUtc = null)
    {
        if (Status != DualCameraProfileStatus.Approved ||
            !string.Equals(SchemaVersion, "1.1.0", StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(ProfileId) || ProfileId.Length > 128 ||
            ProfileId.Any(char.IsControl) ||
            string.IsNullOrWhiteSpace(Version) || Version.Length > 64 ||
            Version.Any(char.IsControl) ||
            string.IsNullOrWhiteSpace(Provenance) || Provenance.Length > 256 ||
            Provenance.Any(char.IsControl) ||
            ValidUntilUtc <= AssessedAtUtc || MeasuredAtUtc > AssessedAtUtc ||
            ExpectedInputWidth <= 0 || ExpectedInputHeight <= 0 ||
            CameraBToCameraA is null || CameraBToCameraA.Count != 9 ||
            CameraBToCameraA.Any(value => !double.IsFinite(value)) ||
            Crop is null || Crop.Count != 4 || Crop.Any(value => value < 0) ||
            Layout is not ("camera-a-left-camera-b-right" or "camera-a-top-camera-b-bottom") ||
            CameraAliases is null || CameraAliases.Count != 2 ||
            !CameraAliases.SequenceEqual(["CAM-A", "CAM-B"], StringComparer.Ordinal) ||
            (nowUtc.HasValue && ValidUntilUtc <= nowUtc.Value))
        {
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidProfile, "The fixed rig profile is draft, malformed, expired, or unsupported.");
        }

        var matrix = CameraBToCameraA;
        var determinant =
            matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7]) -
            matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6]) +
            matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
        if (!double.IsFinite(determinant) || Math.Abs(determinant) < 1e-12)
        {
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidProfile, "The fixed rig transform is singular.");
        }
    }
}

public sealed record DualCameraCaptureRequest
{
    public required CameraOperatingMode Mode { get; init; }

    public required DualCameraExecutionEnvironment ExecutionEnvironment { get; init; }

    public required DualCameraRigProfile Profile { get; init; }

    public DualCameraTestFault TestFault { get; init; }

    public Guid? TransactionId { get; init; }

    public HardwareDualOperatorConfirmations? HardwareConfirmations { get; init; }

    public HardwareDualCaptureProfile? HardwareCaptureProfile { get; init; }

    public static DualCameraCaptureRequest CreateTestSynthetic(DualCameraRigProfile profile) => new()
    {
        Mode = CameraOperatingMode.DualCamera,
        ExecutionEnvironment = DualCameraExecutionEnvironment.TestSynthetic,
        Profile = profile,
        TestFault = DualCameraTestFault.None,
    };

    public static DualCameraCaptureRequest CreateHardwareDual(
        DualCameraRigProfile profile,
        HardwareDualCaptureProfile captureProfile,
        HardwareDualOperatorConfirmations confirmations,
        Guid? transactionId = null) => new()
    {
        Mode = CameraOperatingMode.DualCamera,
        ExecutionEnvironment = DualCameraExecutionEnvironment.HardwareDual,
        Profile = profile,
        HardwareCaptureProfile = captureProfile,
        TransactionId = transactionId,
        HardwareConfirmations = confirmations,
    };
}

public sealed record CanonicalJpegOriginal(
    string Alias,
    string Path,
    long SizeBytes,
    string Sha256,
    int Width,
    int Height,
    bool IsCanonicalJpeg);

public sealed record DualCameraCaptureResult(
    Guid TransactionId,
    IReadOnlyList<CanonicalJpegOriginal> Originals,
    bool Succeeded,
    DualCameraFailureCode FailureCode,
    DualHardwareCaptureEvidence? HardwareEvidence = null);

public sealed record OfflineStitchArtifact(
    string OutputPath,
    int Width,
    int Height,
    string ProfileId);

public sealed record DualCameraStitchResult(
    Guid JobId,
    string OutputPath,
    bool Succeeded,
    DualCameraFailureCode FailureCode,
    string? FailureReason);

public sealed record DualCameraExportResult(
    Guid JobId,
    string? OutputPath,
    bool Succeeded,
    DualCameraFailureCode FailureCode,
    string? FailureReason);

public sealed record DualCameraStageRecord(
    DualCameraProductStage Stage,
    DualCameraStageStatus Status,
    string? Detail);

public sealed record DualCameraProductState
{
    public required CameraOperatingMode Mode { get; init; }

    public required DualCameraExecutionEnvironment ExecutionEnvironment { get; init; }

    public required Guid TransactionId { get; init; }

    public required string ProfileId { get; init; }

    public required string ProfileVersion { get; init; }

    public required DualCameraIdentitySnapshot IdentitySnapshot { get; init; }

    public required bool IsActive { get; init; }

    public required IReadOnlyList<DualCameraStageRecord> Stages { get; init; }

    public required DualCameraCaptureResult? Capture { get; init; }

    public required DualCameraStitchResult? Stitch { get; init; }

    public required IReadOnlyList<DualCameraStitchResult> StitchJobs { get; init; }

    public required DualCameraExportResult? Export { get; init; }

    public required DualCameraFailureCode FailureCode { get; init; }

    public required string? FailureReason { get; init; }

    public required int AutomaticRetryCount { get; init; }
}

public interface ITestSyntheticCamera
{
    Task<string> CaptureAsync(
        string alias,
        Guid transactionId,
        string destinationPath,
        CancellationToken cancellationToken);
}

public sealed record DualCameraCaptureSourceRequest(
    Guid TransactionId,
    string TransactionDirectory,
    DualCameraIdentitySnapshot IdentitySnapshot,
    DualCameraRigProfile ProfileSnapshot,
    HardwareDualCaptureProfile? HardwareCaptureProfileSnapshot,
    HardwareDualOperatorConfirmations? HardwareConfirmations,
    DualCameraTestFault TestFault,
    DateTimeOffset StartedAtUtc,
    DateTimeOffset WatchdogDeadlineUtc);

public sealed record DualCameraCaptureSourceResult(
    IReadOnlyList<(string Alias, string Path)> RetainedOriginals,
    bool Succeeded,
    DualCameraFailureCode FailureCode,
    string? FailureReason,
    DualHardwareCaptureEvidence? HardwareEvidence = null);

public interface IDualCameraCaptureSource
{
    DualCameraExecutionEnvironment Environment { get; }

    Task<DualCameraCaptureSourceResult> CapturePairAsync(
        DualCameraCaptureSourceRequest request,
        CancellationToken cancellationToken);
}

public interface IRecoverableDualCameraCaptureSource
{
    Task<DualCameraCaptureSourceResult> RecoverPairAsync(
        Guid transactionId,
        CancellationToken cancellationToken);
}

public interface IOfflineStitcherAdapter
{
    Task ValidateCanonicalJpegAsync(
        string jpegPath,
        int expectedWidth,
        int expectedHeight,
        CancellationToken cancellationToken);

    Task<OfflineStitchArtifact> StitchAsync(
        IReadOnlyList<CanonicalJpegOriginal> originals,
        string outputJobDirectory,
        DualCameraRigProfile profile,
        CancellationToken cancellationToken);

    Task ExportAsync(string stitchedJpeg, string destinationJpeg, CancellationToken cancellationToken);
}

public interface IDualCameraProductFlow
{
    event EventHandler<DualCameraProductState>? StateChanged;

    event EventHandler<DualCameraIdentitySnapshot>? IdentityChanged;

    DualCameraProductState? Current { get; }

    DualCameraExecutionEnvironment ExecutionEnvironment { get; }

    DualCameraIdentitySnapshot IdentitySnapshot { get; }

    Task<DualCameraProductState> CaptureAndStitchAsync(
        DualCameraCaptureRequest request,
        CancellationToken cancellationToken = default);

    Task<DualCameraProductState> RecoverAndStitchAsync(
        Guid transactionId,
        CancellationToken cancellationToken = default);

    Task<DualCameraProductState> RestitchAsync(CancellationToken cancellationToken = default);

    Task<DualCameraProductState> ExportAsync(
        string fixedLocalDirectory,
        CancellationToken cancellationToken = default);
}

public sealed class DualCameraFlowException : Exception
{
    public DualCameraFlowException(DualCameraFailureCode code, string message, Exception? innerException = null)
        : base(message, innerException)
    {
        Code = code;
    }

    public DualCameraFailureCode Code { get; }
}
