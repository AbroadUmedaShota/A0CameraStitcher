using System.Text.Json.Serialization;

namespace A0CameraStitcher.M3.Foundation.DualCamera;

public sealed record HardwareDualOperatorConfirmations(
    bool IdentitySnapshotApproved,
    bool CaptureProfileFrozen,
    bool RigProfileFrozen,
    bool LiveViewStoppedAndClosed,
    bool BothCardsConfirmedEmpty)
{
    [JsonIgnore]
    public bool AllConfirmed => IdentitySnapshotApproved && CaptureProfileFrozen && RigProfileFrozen &&
        LiveViewStoppedAndClosed && BothCardsConfirmedEmpty;
}

public sealed record HardwareDualBodyCaptureSettings(
    string Alias,
    string ImageArea,
    string FileFormat,
    string JpegQuality,
    string ImageSize,
    string ExposureMode,
    bool AutoIsoEnabled,
    string FocusMode,
    string WhiteBalanceMode,
    bool VibrationReductionEnabled);

public sealed record HardwareDualCaptureProfile(
    string ProfileId,
    string Version,
    string SchemaVersion,
    DualCameraProfileStatus Status,
    DateTimeOffset ApprovedAtUtc,
    DateTimeOffset ValidUntilUtc,
    IReadOnlyList<HardwareDualBodyCaptureSettings> Bodies)
{
    public static HardwareDualCaptureProfile ApprovedSynthetic() => new(
        "anonymous-hardware-fake-capture-v1",
        "1",
        "a0.hardware-dual-capture-profile.v1",
        DualCameraProfileStatus.Approved,
        DateTimeOffset.FromUnixTimeSeconds(200),
        DateTimeOffset.FromUnixTimeSeconds(4_102_444_800),
        Array.AsReadOnly([
            Fixed("CAM-A"),
            Fixed("CAM-B"),
        ]));

    public void Validate(DateTimeOffset nowUtc)
    {
        if (Status != DualCameraProfileStatus.Approved ||
            !string.Equals(SchemaVersion, "a0.hardware-dual-capture-profile.v1", StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(ProfileId) || ProfileId.Length > 128 || ProfileId.Any(char.IsControl) ||
            string.IsNullOrWhiteSpace(Version) || Version.Length > 64 || Version.Any(char.IsControl) ||
            ApprovedAtUtc >= ValidUntilUtc || ValidUntilUtc <= nowUtc || Bodies is null || Bodies.Count != 2)
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidProfile, "The HardwareDual capture profile is unapproved, expired, or malformed.");

        var expected = new[] { Fixed("CAM-A"), Fixed("CAM-B") };
        if (!Bodies.SequenceEqual(expected))
            throw new DualCameraFlowException(DualCameraFailureCode.ProfileMismatch, "CAM-A/B capture settings do not match the approved frozen FX JPEG Fine L profile.");
    }

    public HardwareDualCaptureProfile Freeze() => this with
    {
        Bodies = Array.AsReadOnly(Bodies.ToArray()),
    };

    private static HardwareDualBodyCaptureSettings Fixed(string alias) => new(
        alias,
        "FX",
        "JPEG",
        "Fine",
        "L",
        "Manual",
        false,
        "Manual",
        "Fixed",
        false);
}

public enum DualHardwareCaptureTerminalState
{
    Succeeded,
    Failed,
    FailedPartial,
    WatchdogExpired,
    ResponseUnknown,
    HardwarePending,
}

public enum DualHardwareDispatchState
{
    Completed,
    ResponseUnknown,
    ConfirmedUndispatched,
}

public sealed record DualHardwareOriginalRecord(
    string Alias,
    string CanonicalOriginalPath,
    bool ExactRecoveredObjectDeleted,
    bool SpoolEmptyAfterDelete);

public sealed record DualHardwareCaptureEvidence(
    DualHardwareCaptureTerminalState TerminalState,
    DualCameraIdentitySnapshot IdentitySnapshot,
    string CaptureProfileId,
    string CaptureProfileVersion,
    string ProfileId,
    string ProfileVersion,
    DateTimeOffset WatchdogStartedAtUtc,
    DateTimeOffset WatchdogDeadlineUtc,
    DateTimeOffset CompletedAtUtc,
    bool WatchdogCompletedInTime,
    bool LiveViewStopAndCloseConfirmed,
    bool ExactDeleteConfirmedForEveryRetainedOriginal,
    bool BothSpoolsEmptyAfter,
    int AutomaticRetryCount);

public sealed record DualHardwareCaptureRequest(
    Guid TransactionId,
    string TransactionDirectory,
    DualCameraIdentitySnapshot IdentitySnapshot,
    HardwareDualCaptureProfile CaptureProfileSnapshot,
    DualCameraRigProfile RigProfileSnapshot,
    HardwareDualOperatorConfirmations OperatorConfirmations,
    DateTimeOffset StartedAtUtc,
    DateTimeOffset WatchdogDeadlineUtc);

public sealed record DualHardwareCaptureResult(
    Guid TransactionId,
    IReadOnlyList<DualHardwareOriginalRecord> Originals,
    DualHardwareCaptureTerminalState TerminalState,
    DualCameraFailureCode FailureCode,
    DualHardwareCaptureEvidence Evidence);

public sealed record DualHardwareDispatchResult(
    DualHardwareDispatchState State,
    DualHardwareCaptureResult? Result);

public enum DualHardwarePairQueryState
{
    NotFound,
    Reserved,
    ClosedBeforeDispatch,
    Terminal,
}

public enum DualHardwareCloseState
{
    ClosedBeforeDispatch,
    ResponseUnknown,
}

public sealed record DualHardwarePairQueryOutcome(
    DualHardwarePairQueryState State,
    DualHardwareCaptureResult? Result);

public interface IDualHardwareCaptureOperations
{
    Task<bool> ReservePairTransactionAsync(Guid transactionId, CancellationToken cancellationToken);

    Task<DualHardwareDispatchResult> StartReservedPairAsync(
        DualHardwareCaptureRequest request,
        CancellationToken cancellationToken);

    Task<DualHardwarePairQueryOutcome> QueryPairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken);

    Task<DualHardwareCloseState> CloseReservedPairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken) =>
        Task.FromResult(DualHardwareCloseState.ResponseUnknown);
}

public enum DualHardwareRecoveryIntent
{
    MayHaveDispatched,
    CloseReservedBeforeDispatch,
}

public interface IDualHardwareRecoveryStore
{
    DualHardwareCaptureRequest? LoadPending();

    void SavePending(DualHardwareCaptureRequest request);

    DualHardwareRecoveryIntent LoadPendingIntent(Guid expectedTransactionId) =>
        DualHardwareRecoveryIntent.MayHaveDispatched;

    void MarkCloseReservedBeforeDispatch(Guid expectedTransactionId) =>
        throw new NotSupportedException(
            "The recovery store cannot persist a pre-dispatch close intent.");

    void ClearPending(Guid expectedTransactionId);
}

public sealed class HardwareDualCaptureSource : IDualCameraCaptureSource, IRecoverableDualCameraCaptureSource
{
    private static readonly TimeSpan Watchdog = TimeSpan.FromSeconds(180);
    private readonly IDualHardwareCaptureOperations? _operations;
    private readonly TimeProvider _timeProvider;
    private readonly IDualHardwareRecoveryStore? _recoveryStore;
    private readonly object _sync = new();
    private readonly HashSet<Guid> _seenTransactions = [];
    private Guid? _responseUnknownTransactionId;
    private DualHardwareCaptureRequest? _responseUnknownRequest;
    private DualHardwareRecoveryIntent _recoveryIntent =
        DualHardwareRecoveryIntent.MayHaveDispatched;

    public HardwareDualCaptureSource(
        IDualHardwareCaptureOperations? operations,
        TimeProvider? timeProvider = null,
        IDualHardwareRecoveryStore? recoveryStore = null)
    {
        _operations = operations;
        _timeProvider = timeProvider ?? TimeProvider.System;
        _recoveryStore = recoveryStore;
        var pending = recoveryStore?.LoadPending();
        if (pending is not null)
        {
            _responseUnknownTransactionId = pending.TransactionId;
            _responseUnknownRequest = pending;
            _recoveryIntent = recoveryStore!.LoadPendingIntent(pending.TransactionId);
            _seenTransactions.Add(pending.TransactionId);
        }
    }

    public DualCameraExecutionEnvironment Environment => DualCameraExecutionEnvironment.HardwareDual;

    public DualHardwareCaptureRequest? PendingRecoveryRequest
    {
        get
        {
            lock (_sync)
            {
                return _responseUnknownRequest;
            }
        }
    }

    public async Task<DualCameraCaptureSourceResult> CapturePairAsync(
        DualCameraCaptureSourceRequest request,
        CancellationToken cancellationToken)
    {
        ValidateBeforeSideEffects(request);
        if (_operations is null)
        {
            return Failed(DualCameraFailureCode.HardwarePending, "HardwareDual provider and Agent operation are not connected.");
        }

        lock (_sync)
        {
            if (_responseUnknownTransactionId.HasValue)
            {
                throw new DualCameraFlowException(
                    DualCameraFailureCode.AgentResponseUnknown,
                    "A prior Agent dispatch remains unknown; use recovery for the saved transaction snapshot.");
            }
            if (!_seenTransactions.Add(request.TransactionId))
            {
                throw new DualCameraFlowException(DualCameraFailureCode.DuplicateStart, "The durable pair transaction ID was already started.");
            }
        }

        if (!await _operations.ReservePairTransactionAsync(request.TransactionId, cancellationToken).ConfigureAwait(false))
        {
            throw new DualCameraFlowException(DualCameraFailureCode.DuplicateStart, "The durable pair transaction reservation was rejected.");
        }

        var hardwareRequest = new DualHardwareCaptureRequest(
            request.TransactionId,
            request.TransactionDirectory,
            request.IdentitySnapshot with { },
            request.HardwareCaptureProfileSnapshot!.Freeze(),
            Freeze(request.ProfileSnapshot),
            request.HardwareConfirmations!,
            request.StartedAtUtc,
            request.StartedAtUtc + Watchdog);
        try
        {
            _recoveryStore?.SavePending(hardwareRequest);
        }
        catch (Exception exception)
        {
            throw new DualCameraFlowException(
                DualCameraFailureCode.AgentResponseUnknown,
                "The frozen HardwareDual transaction snapshot could not be persisted; Agent dispatch was not attempted.",
                exception);
        }
        lock (_sync)
        {
            _responseUnknownTransactionId = request.TransactionId;
            _responseUnknownRequest = hardwareRequest;
        }
        DualHardwareDispatchResult dispatch;
        try
        {
            dispatch = await _operations.StartReservedPairAsync(hardwareRequest, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception)
        {
            return Failed(DualCameraFailureCode.AgentResponseUnknown, "Agent dispatch outcome is unknown; only the durable transaction may be queried.");
        }
        if (dispatch.State == DualHardwareDispatchState.ConfirmedUndispatched)
        {
            return await CloseConfirmedUndispatchedAsync(
                    hardwareRequest,
                    persistIntent: true,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        DualHardwarePairQueryOutcome queryOutcome;
        try
        {
            queryOutcome = dispatch.State == DualHardwareDispatchState.Completed
                ? new(DualHardwarePairQueryState.Terminal, dispatch.Result)
                : await _operations.QueryPairTransactionAsync(request.TransactionId, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception)
        {
            return Failed(DualCameraFailureCode.AgentResponseUnknown, "Agent query outcome is unknown; no capture was reserved or dispatched again.");
        }
        if (queryOutcome.State != DualHardwarePairQueryState.Terminal ||
            queryOutcome.Result is null || !IsActualTerminal(queryOutcome.Result.TerminalState))
        {
            return Failed(DualCameraFailureCode.AgentResponseUnknown, "Agent response is unknown; only this transaction may be queried and no new capture is allowed.");
        }
        return CompleteRecoveredResult(
            hardwareRequest,
            queryOutcome.Result,
            recoveryRequired: dispatch.State == DualHardwareDispatchState.ResponseUnknown);
    }

    public async Task<DualCameraCaptureSourceResult> RecoverPairAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        if (_operations is null)
            return PendingRecoveryRequest?.TransactionId == transactionId
                ? Failed(DualCameraFailureCode.AgentResponseUnknown, "The durable HardwareDual transaction remains pending until Agent query operations are connected.")
                : Failed(DualCameraFailureCode.HardwarePending, "HardwareDual provider and Agent operation are not connected.");

        DualHardwareCaptureRequest hardwareRequest;
        DualHardwareRecoveryIntent recoveryIntent;
        lock (_sync)
        {
            if (_responseUnknownTransactionId != transactionId || _responseUnknownRequest is null)
                throw new DualCameraFlowException(
                    DualCameraFailureCode.AgentResponseUnknown,
                    "No response-unknown transaction snapshot is saved for recovery.");
            hardwareRequest = _responseUnknownRequest;
            recoveryIntent = _recoveryIntent;
        }

        if (recoveryIntent == DualHardwareRecoveryIntent.CloseReservedBeforeDispatch)
        {
            return await CloseConfirmedUndispatchedAsync(
                    hardwareRequest,
                    persistIntent: false,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        DualHardwarePairQueryOutcome queryOutcome;
        try
        {
            queryOutcome = await _operations.QueryPairTransactionAsync(transactionId, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception)
        {
            return Failed(DualCameraFailureCode.AgentResponseUnknown, "Agent query outcome remains unknown; no capture was reserved or dispatched again.");
        }
        if (queryOutcome.State != DualHardwarePairQueryState.Terminal ||
            queryOutcome.Result is null || !IsActualTerminal(queryOutcome.Result.TerminalState))
            return Failed(DualCameraFailureCode.AgentResponseUnknown, "Agent response remains unknown; no capture was reserved or dispatched again.");

        return CompleteRecoveredResult(hardwareRequest, queryOutcome.Result, recoveryRequired: true);
    }

    private async Task<DualCameraCaptureSourceResult> CloseConfirmedUndispatchedAsync(
        DualHardwareCaptureRequest hardwareRequest,
        bool persistIntent,
        CancellationToken cancellationToken)
    {
        if (persistIntent)
        {
            if (_recoveryStore is null)
            {
                return Failed(
                    DualCameraFailureCode.AgentResponseUnknown,
                    "The confirmed-undispatched cleanup intent has no durable recovery store; the reservation remains locked and no close was attempted.");
            }
            try
            {
                _recoveryStore.MarkCloseReservedBeforeDispatch(
                    hardwareRequest.TransactionId);
            }
            catch (Exception)
            {
                return Failed(
                    DualCameraFailureCode.AgentResponseUnknown,
                    "The confirmed-undispatched cleanup intent could not be persisted; the reservation remains locked and no close was attempted.");
            }
            lock (_sync)
            {
                _recoveryIntent =
                    DualHardwareRecoveryIntent.CloseReservedBeforeDispatch;
            }
        }

        DualHardwareCloseState closeState;
        try
        {
            closeState = await _operations!
                .CloseReservedPairTransactionAsync(
                    hardwareRequest.TransactionId,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception)
        {
            closeState = DualHardwareCloseState.ResponseUnknown;
        }
        if (closeState != DualHardwareCloseState.ClosedBeforeDispatch)
        {
            return Failed(
                DualCameraFailureCode.AgentResponseUnknown,
                "The exact Reserved transaction close is unconfirmed; only the same transaction close may be retried.");
        }
        if (!TryClearPending(hardwareRequest.TransactionId, out var clearFailure))
        {
            return Failed(DualCameraFailureCode.AgentResponseUnknown, clearFailure!);
        }
        return Failed(
            DualCameraFailureCode.HardwarePending,
            "Agent start failed before dispatch; the exact Reserved transaction was durably closed without capture.");
    }

    private DualCameraCaptureSourceResult CompleteRecoveredResult(
        DualHardwareCaptureRequest hardwareRequest,
        DualHardwareCaptureResult result,
        bool recoveryRequired)
    {
        if (!IsActualTerminal(result.TerminalState))
        {
            return Failed(
                DualCameraFailureCode.AgentResponseUnknown,
                "Agent transaction is not terminal; the durable snapshot remains pending and no capture was reserved or dispatched again.");
        }
        try
        {
            ValidateAnonymousResult(hardwareRequest, result);
        }
        catch (DualCameraFlowException exception)
        {
            if (!recoveryRequired)
            {
                if (!TryClearPending(hardwareRequest.TransactionId, out var clearFailure))
                {
                    return new(
                        result.Originals
                            .Where(item => IsExpectedOriginalPath(hardwareRequest, item))
                            .Select(item => (item.Alias, item.CanonicalOriginalPath)).ToArray(),
                        false,
                        DualCameraFailureCode.AgentResponseUnknown,
                        clearFailure,
                        result.Evidence);
                }
                return new(
                    result.Originals
                        .Where(item => IsExpectedOriginalPath(hardwareRequest, item))
                        .Select(item => (item.Alias, item.CanonicalOriginalPath)).ToArray(),
                    false,
                    exception.Code,
                    exception.Message,
                    result.Evidence);
            }
            return new(
                result.Originals
                    .Where(item => IsExpectedOriginalPath(hardwareRequest, item))
                    .Select(item => (item.Alias, item.CanonicalOriginalPath)).ToArray(),
                false,
                DualCameraFailureCode.AgentResponseUnknown,
                $"Recovered Agent evidence did not match the durable transaction snapshot: {exception.Message}",
                result.Evidence);
        }
        if (!TryClearPending(hardwareRequest.TransactionId, out var failureReason))
        {
            return new(
                result.Originals
                    .Where(item => IsExpectedOriginalPath(hardwareRequest, item))
                    .Select(item => (item.Alias, item.CanonicalOriginalPath)).ToArray(),
                false,
                DualCameraFailureCode.AgentResponseUnknown,
                failureReason,
                result.Evidence);
        }
        return new(
            result.Originals.Select(item => (item.Alias, item.CanonicalOriginalPath)).ToArray(),
            result.TerminalState == DualHardwareCaptureTerminalState.Succeeded,
            result.FailureCode,
            result.FailureCode == DualCameraFailureCode.None
                ? null
                : $"HardwareDual capture ended with {result.FailureCode}.",
            result.Evidence);
    }

    private bool TryClearPending(Guid transactionId, out string? failureReason)
    {
        try
        {
            _recoveryStore?.ClearPending(transactionId);
        }
        catch (Exception exception)
        {
            failureReason = $"Validated Agent terminal state could not clear the durable recovery snapshot: {exception.Message}";
            return false;
        }
        lock (_sync)
        {
            if (_responseUnknownTransactionId == transactionId)
            {
                _responseUnknownTransactionId = null;
                _responseUnknownRequest = null;
                _recoveryIntent = DualHardwareRecoveryIntent.MayHaveDispatched;
            }
        }
        failureReason = null;
        return true;
    }

    private static void ValidateBeforeSideEffects(DualCameraCaptureSourceRequest request)
    {
        if (!request.IdentitySnapshot.IsReady)
            throw new DualCameraFlowException(DualCameraFailureCode.IdentityNotReady, "HardwareDual identity is not Ready.");
        request.ProfileSnapshot.Validate(request.StartedAtUtc);
        if (request.HardwareCaptureProfileSnapshot is null)
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidProfile, "A HardwareDual capture profile snapshot is required.");
        request.HardwareCaptureProfileSnapshot.Validate(request.StartedAtUtc);
        if (request.HardwareConfirmations?.AllConfirmed != true)
            throw new DualCameraFlowException(DualCameraFailureCode.LiveViewStopFailed, "All HardwareDual operator confirmations are required before Agent dispatch.");
    }

    private void ValidateAnonymousResult(DualHardwareCaptureRequest request, DualHardwareCaptureResult result)
    {
        if (result.TransactionId != request.TransactionId || result.Evidence.AutomaticRetryCount != 0 ||
            result.Evidence.TerminalState != result.TerminalState ||
            result.Evidence.IdentitySnapshot != request.IdentitySnapshot ||
            result.Evidence.CaptureProfileId != request.CaptureProfileSnapshot.ProfileId ||
            result.Evidence.CaptureProfileVersion != request.CaptureProfileSnapshot.Version ||
            result.Evidence.ProfileId != request.RigProfileSnapshot.ProfileId ||
            result.Evidence.ProfileVersion != request.RigProfileSnapshot.Version ||
            result.Evidence.WatchdogStartedAtUtc != request.StartedAtUtc ||
            result.Evidence.WatchdogDeadlineUtc != request.WatchdogDeadlineUtc ||
            result.Evidence.WatchdogDeadlineUtc - result.Evidence.WatchdogStartedAtUtc != Watchdog ||
            (result.TerminalState == DualHardwareCaptureTerminalState.Succeeded) !=
                (result.FailureCode == DualCameraFailureCode.None))
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "HardwareDual Agent returned mismatched transaction snapshot evidence.");
        request.CaptureProfileSnapshot.Validate(request.StartedAtUtc);
        foreach (var item in result.Originals)
        {
            var expectedPath = Path.GetFullPath(Path.Combine(request.TransactionDirectory, item.Alias, "original.jpg"));
            if (!string.Equals(Path.GetFullPath(item.CanonicalOriginalPath), expectedPath, StringComparison.OrdinalIgnoreCase))
                throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "HardwareDual Agent returned an original outside the reserved transaction path.");
            EnsureNoReparsePoint(expectedPath, request.TransactionDirectory);
        }
        if (!result.Evidence.LiveViewStopAndCloseConfirmed)
            throw new DualCameraFlowException(DualCameraFailureCode.LiveViewStopFailed, "Live View stop/close evidence was not confirmed.");
        if (result.TerminalState == DualHardwareCaptureTerminalState.WatchdogExpired ||
            !result.Evidence.WatchdogCompletedInTime ||
            result.Evidence.CompletedAtUtc > result.Evidence.WatchdogDeadlineUtc ||
            _timeProvider.GetUtcNow() > result.Evidence.WatchdogDeadlineUtc)
            throw new DualCameraFlowException(DualCameraFailureCode.WatchdogExpired, "HardwareDual watchdog expired; no next transport or capture is allowed.");
        if (result.Originals.Any(item => !item.ExactRecoveredObjectDeleted || !item.SpoolEmptyAfterDelete) ||
            !result.Evidence.ExactDeleteConfirmedForEveryRetainedOriginal || !result.Evidence.BothSpoolsEmptyAfter)
            throw new DualCameraFlowException(DualCameraFailureCode.SpoolNotEmpty, "Exact-delete or empty-after evidence is incomplete.");
        if (result.Originals.Select(item => item.Alias).Distinct(StringComparer.Ordinal).Count() != result.Originals.Count ||
            result.Originals.Any(item => item.Alias is not ("CAM-A" or "CAM-B")))
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "HardwareDual Agent returned an invalid anonymous original set.");
    }

    private static DualCameraRigProfile Freeze(DualCameraRigProfile profile) => profile with
    {
        CameraBToCameraA = Array.AsReadOnly(profile.CameraBToCameraA.ToArray()),
        Crop = Array.AsReadOnly(profile.Crop.ToArray()),
        CameraAliases = Array.AsReadOnly(profile.CameraAliases.ToArray()),
    };

    private static bool IsActualTerminal(DualHardwareCaptureTerminalState state) => state is
        DualHardwareCaptureTerminalState.Succeeded or
        DualHardwareCaptureTerminalState.Failed or
        DualHardwareCaptureTerminalState.FailedPartial or
        DualHardwareCaptureTerminalState.WatchdogExpired;

    private static void EnsureNoReparsePoint(string filePath, string transactionDirectory)
    {
        var root = Path.GetFullPath(transactionDirectory);
        if (root.StartsWith("\\\\", StringComparison.Ordinal) || root.StartsWith("//", StringComparison.Ordinal) ||
            root.StartsWith("\\\\?\\", StringComparison.Ordinal) || root.StartsWith("\\\\.\\", StringComparison.Ordinal))
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "Transaction directory must be a normal local path.");
        var current = root;
        if (Directory.Exists(current) && (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "Transaction directory may not be a reparse point.");
        var relative = Path.GetRelativePath(root, filePath);
        if (relative.StartsWith("..", StringComparison.Ordinal) || Path.IsPathRooted(relative))
            throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "Original path escaped the transaction directory.");
        foreach (var segment in relative.Split(Path.DirectorySeparatorChar, StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, segment);
            if (File.Exists(current) || Directory.Exists(current))
            {
                if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                    throw new DualCameraFlowException(DualCameraFailureCode.InvalidOriginal, "Original path may not contain a reparse point.");
            }
        }
    }

    private static bool IsExpectedOriginalPath(DualHardwareCaptureRequest request, DualHardwareOriginalRecord item)
    {
        if (item.Alias is not ("CAM-A" or "CAM-B")) return false;
        var expected = Path.GetFullPath(Path.Combine(request.TransactionDirectory, item.Alias, "original.jpg"));
        return string.Equals(Path.GetFullPath(item.CanonicalOriginalPath), expected, StringComparison.OrdinalIgnoreCase);
    }

    private static DualCameraCaptureSourceResult Failed(DualCameraFailureCode code, string reason) =>
        new([], false, code, reason);
}

internal sealed class TestSyntheticCaptureSource(ITestSyntheticCamera camera) : IDualCameraCaptureSource
{
    public DualCameraExecutionEnvironment Environment => DualCameraExecutionEnvironment.TestSynthetic;

    public async Task<DualCameraCaptureSourceResult> CapturePairAsync(
        DualCameraCaptureSourceRequest request,
        CancellationToken cancellationToken)
    {
        if (request.TestFault == DualCameraTestFault.FailBeforeCapture)
            return new([], false, DualCameraFailureCode.LiveViewStopFailed, "TestSynthetic Live View stop failed before any capture call.");
        var retained = new List<(string Alias, string Path)>();
        foreach (var alias in new[] { "CAM-A", "CAM-B" })
        {
            var failure = alias == "CAM-A" ? DualCameraTestFault.FailCaptureCameraA : DualCameraTestFault.FailCaptureCameraB;
            if (request.TestFault == failure)
                return new(retained, false, alias == "CAM-A" ? DualCameraFailureCode.CaptureCameraA : DualCameraFailureCode.CaptureCameraB, $"{alias} TestSynthetic capture failed before file generation.");
            var path = Path.Combine(request.TransactionDirectory, alias, "original.jpg");
            try
            {
                await camera.CaptureAsync(alias, request.TransactionId, path, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception)
            {
                return new(retained, false,
                    alias == "CAM-A" ? DualCameraFailureCode.CaptureCameraA : DualCameraFailureCode.CaptureCameraB,
                    $"{alias} capture failed before canonical validation.");
            }
            retained.Add((alias, path));
            if (alias == "CAM-A" && request.TestFault == DualCameraTestFault.InterruptAfterCameraA)
                return new(retained, false, DualCameraFailureCode.Interrupted, "TestSynthetic execution stopped after CAM-A canonical verification.");
        }
        return new(retained, true, DualCameraFailureCode.None, null);
    }
}
