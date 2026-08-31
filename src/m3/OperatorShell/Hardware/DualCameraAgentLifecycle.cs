using System.Diagnostics;
using System.IO;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

/// <summary>
/// Owns the .NET client/process lifecycle for the HardwareDual native Camera Agent
/// (A0CameraStitcher.DualCameraAgent.exe). One instance is created once by the WPF
/// production composition root and reused across HardwareDual transactions.
///
/// It starts no process until the first typed operation (Reserve/Start/Query) is
/// actually invoked. Identity gating happens upstream in HardwareDualCaptureSource,
/// so while DualCamera identity is Pending, this class is never called and touches
/// no process or camera.
///
/// The Agent process is long-lived (persistent, not --serve-once) and self-terminates
/// on the native side after a bounded maximum lifetime. This class never kills a
/// process once request dispatch may have occurred; the only automatic recovery it
/// performs is spinning up a fresh process (with a freshly generated unique pipe name)
/// once the previous process has already exited, and it only ever resumes with a
/// same-transaction query -- Reserve and Start are never re-sent automatically. That
/// invariant is guaranteed by the caller (HardwareDualCaptureSource / DualCameraProductFlow,
/// unmodified Foundation code): only DualCameraProductFlow.RecoverAndStitchAsync ever
/// calls QueryPairTransactionAsync again for an already-dispatched transaction.
/// </summary>
public sealed class DualCameraAgentLifecycle :
    IDualHardwareCaptureOperations,
    IDualHardwareCaptureRecoveryOnlyOperations,
    IHardwareCameraAgentTransport,
    IHardwareCameraAgentProcessLifetime,
    IAsyncDisposable
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);

    // Matches ServeOnceHardwareCameraAgentOperations.CaptureResponseTimeout (Single):
    // longer than the native shared 180s dispatch watchdog so the client never times
    // out a call before the Agent's own watchdog would already have resolved it.
    private static readonly TimeSpan ResponseTimeout = TimeSpan.FromSeconds(240);
    private static readonly TimeSpan BindingShutdownExitTimeout = TimeSpan.FromSeconds(5);

    private readonly string _agentExecutablePath;
    private readonly string _pairJournalRootPath;
    private readonly string _approvedCaptureProfilePath;
    private readonly string _dualIdentityProofPath;
    private readonly string? _wpdCameraMapPath;
    private readonly SemaphoreSlim _operationGate = new(1, 1);

    private Process? _process;

    // Assigned and never read back: starting this read continuously drains the
    // child's redirected stdout so its OS pipe buffer cannot fill and block the
    // Agent process. Stdout is not used for diagnostics -- only stderr is captured
    // and surfaced on failure (see CaptureExitDiagnosticsAsync).
    private Task<string>? _standardOutput;
    private Task<string>? _standardError;
    private string? _pipeName;
    private string? _bindingPipeName;
    private DualHardwareCameraAgentOperations? _wireOperations;
    private IHardwareCameraAgentTransport? _bindingWireTransport;
    private bool _bindingSessionMayNeedCleanup;
    private bool _bindingCancellationResponseReceived;
    private bool _bindingCancellationSucceeded;
    // Once activation succeeds the binding pipe is intentionally unavailable.
    // The capture host must therefore be allowed to reach its own terminal state;
    // shutdown must never cancel it or merely detach from a live process.
    private bool _captureHostActivated;
    private bool _disposed;
    private long _processGeneration;

    public DualCameraAgentLifecycle(
        string agentExecutablePath,
        string pairJournalRootPath,
        string approvedCaptureProfilePath,
        string dualIdentityProofPath,
        string? wpdCameraMapPath = null)
    {
        if (string.IsNullOrWhiteSpace(agentExecutablePath))
        {
            throw new ArgumentException("A Dual Camera Agent executable path is required.", nameof(agentExecutablePath));
        }
        if (string.IsNullOrWhiteSpace(pairJournalRootPath))
        {
            throw new ArgumentException("A pair journal root path is required.", nameof(pairJournalRootPath));
        }
        if (string.IsNullOrWhiteSpace(approvedCaptureProfilePath))
        {
            throw new ArgumentException("An approved capture profile path is required.", nameof(approvedCaptureProfilePath));
        }
        if (string.IsNullOrWhiteSpace(dualIdentityProofPath))
        {
            throw new ArgumentException("A dual identity proof path is required.", nameof(dualIdentityProofPath));
        }

        _agentExecutablePath = Path.GetFullPath(agentExecutablePath);
        _pairJournalRootPath = Path.GetFullPath(pairJournalRootPath);
        _approvedCaptureProfilePath = Path.GetFullPath(approvedCaptureProfilePath);
        _dualIdentityProofPath = Path.GetFullPath(dualIdentityProofPath);
        _wpdCameraMapPath = string.IsNullOrWhiteSpace(wpdCameraMapPath)
            ? null
            : Path.GetFullPath(wpdCameraMapPath);
    }

    public string AgentExecutablePath => _agentExecutablePath;

    public long CurrentProcessGeneration => Interlocked.Read(ref _processGeneration);

    /// <summary>
    /// Exit code observed during an orderly lifecycle shutdown. A non-zero code is
    /// retained for truthful diagnostics, but an already-exited capture host no
    /// longer holds the exclusive hardware lease indefinitely.
    /// </summary>
    public int? LastObservedAgentExitCode { get; private set; }

    public bool IsProcessGenerationAlive(long processGeneration)
    {
        if (processGeneration <= 0 || processGeneration != CurrentProcessGeneration)
        {
            return false;
        }

        var process = Volatile.Read(ref _process);
        try
        {
            return process is { HasExited: false } && processGeneration == CurrentProcessGeneration;
        }
        catch (Exception exception) when (exception is InvalidOperationException or ObjectDisposedException)
        {
            return false;
        }
    }

    public bool AgentExecutableAvailable
    {
        get
        {
            try
            {
                if (!Path.IsPathFullyQualified(_agentExecutablePath) ||
                    IsUncPath(_agentExecutablePath) ||
                    !File.Exists(_agentExecutablePath))
                {
                    return false;
                }

                WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_agentExecutablePath);
                var attributes = File.GetAttributes(_agentExecutablePath);
                return (attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) == 0;
            }
            catch (Exception exception) when (
                exception is IOException or UnauthorizedAccessException or ArgumentException or
                NotSupportedException or InvalidDataException)
            {
                return false;
            }
        }
    }

    public Task<bool> ReservePairTransactionAsync(Guid transactionId, CancellationToken cancellationToken) =>
        RunSerializedAsync(
            (operations, token) => operations.ReservePairTransactionAsync(transactionId, token),
            cancellationToken);

    public async Task<DualHardwareDispatchResult> StartReservedPairAsync(
        DualHardwareCaptureRequest request,
        CancellationToken cancellationToken)
    {
        try
        {
            return await RunSerializedAsync(
                    (operations, token) => operations.StartReservedPairAsync(request, token),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (HardwareCameraAgentLaunchException exception)
        {
            return new(
                exception.RequestMayHaveBeenDispatched
                    ? DualHardwareDispatchState.ResponseUnknown
                    : DualHardwareDispatchState.ConfirmedUndispatched,
                null);
        }
    }

    public async Task<DualHardwareCaptureRecoveryOnlyDispatchResult> StartReservedCaptureRecoveryOnlyAsync(
        DualHardwareCaptureRecoveryOnlyRequest request,
        CancellationToken cancellationToken)
    {
        try
        {
            return await RunSerializedAsync(
                    (operations, token) => operations.StartReservedCaptureRecoveryOnlyAsync(request, token),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (HardwareCameraAgentLaunchException exception)
        {
            return new(
                exception.RequestMayHaveBeenDispatched
                    ? DualHardwareDispatchState.ResponseUnknown
                    : DualHardwareDispatchState.ConfirmedUndispatched,
                null);
        }
    }

    public async Task EnsureCaptureRecoveryOnlyAvailableAsync(CancellationToken cancellationToken)
    {
        await RunSerializedAsync(
                async (operations, token) =>
                {
                    await operations.EnsureCaptureRecoveryOnlyAvailableAsync(token).ConfigureAwait(false);
                    return true;
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    public Task<DualHardwarePairQueryOutcome> QueryPairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken) =>
        RunSerializedAsync(
            (operations, token) => operations.QueryPairTransactionAsync(transactionId, token),
            cancellationToken);

    public Task<DualHardwareCaptureRecoveryOnlyPairQueryOutcome> QueryCaptureRecoveryOnlyTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken) =>
        RunSerializedAsync(
            (operations, token) => operations.QueryCaptureRecoveryOnlyTransactionAsync(transactionId, token),
            cancellationToken);

    public async Task<DualHardwareCloseState> CloseCaptureRecoveryOnlyReservedPairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        try
        {
            return await RunSerializedAsync(
                    (operations, token) => operations.CloseCaptureRecoveryOnlyReservedPairTransactionAsync(
                        transactionId,
                        token),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (HardwareCameraAgentLaunchException)
        {
            return DualHardwareCloseState.ResponseUnknown;
        }
    }

    /// <summary>
    /// Serves the operator binding protocol through the same child process as the
    /// subsequent capture protocol. A WPD map is mandatory on this path; capture-only
    /// callers that do not use session binding retain their existing behavior.
    /// </summary>
    public async Task<string> SendAsync(string requestJson, CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(requestJson);
        ObjectDisposedException.ThrowIf(_disposed, this);
        await _operationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_wpdCameraMapPath is null)
            {
                throw new HardwareCameraAgentLaunchException(
                    "Dual binding requires an existing WPD camera map.",
                    requestMayHaveBeenDispatched: false);
            }

            var bindingTransport = EnsureBindingProcessStarted();
            var operation = ReadBindingOperation(requestJson);
            var cleanupRequiredBeforeRequest = _bindingSessionMayNeedCleanup;
            if (operation == DualBindingCameraAgentProtocol.Operations.BeginBinding)
            {
                // Until a complete response proves otherwise, begin-binding may
                // have reached Native and opened the retained SDK session.
                _bindingSessionMayNeedCleanup = true;
            }

            try
            {
                var responseJson = await bindingTransport.SendAsync(requestJson, cancellationToken).ConfigureAwait(false);
                RecordBindingLifecycleResponse(operation, requestJson, responseJson);
                return responseJson;
            }
            catch (HardwareCameraAgentConnectException)
                when (operation == DualBindingCameraAgentProtocol.Operations.BeginBinding)
            {
                // Server identity is checked before any request byte is written.
                // A connect/identity failure therefore cannot have started the
                // new binding; preserve only an older session, if one existed.
                _bindingSessionMayNeedCleanup = cleanupRequiredBeforeRequest;
                throw;
            }
        }
        finally
        {
            _operationGate.Release();
        }
    }

    public async Task<DualHardwareCloseState> CloseReservedPairTransactionAsync(
        Guid transactionId,
        CancellationToken cancellationToken)
    {
        try
        {
            return await RunSerializedAsync(
                    (operations, token) => operations.CloseReservedPairTransactionAsync(
                        transactionId,
                        token),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch (HardwareCameraAgentLaunchException)
        {
            return DualHardwareCloseState.ResponseUnknown;
        }
    }

    private async Task<T> RunSerializedAsync<T>(
        Func<DualHardwareCameraAgentOperations, CancellationToken, Task<T>> operation,
        CancellationToken cancellationToken)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        await _operationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var wireOperations = EnsureProcessStarted();
            try
            {
                return await operation(wireOperations, cancellationToken).ConfigureAwait(false);
            }
            catch (HardwareCameraAgentConnectException exception)
            {
                // A connect failure means the pipe was never reached, so unlike an
                // incomplete response this is never ambiguous -- but if the process
                // has already exited (e.g. Native failed fast on bad/missing
                // arguments), its exit code and stderr are the only diagnosable
                // reason available. Without this, the operator only ever sees a
                // generic "connection failed" after the full connect timeout.
                throw await CreateConnectFailureAsync(exception).ConfigureAwait(false);
            }
            catch (IOException exception)
            {
                throw await CreatePipeFailureAsync(exception).ConfigureAwait(false);
            }
        }
        finally
        {
            _operationGate.Release();
        }
    }

    /// <summary>
    /// Returns the wire-protocol client bound to a live Agent process, starting one
    /// with a freshly generated unique pipe name if none is currently alive. A live
    /// process (and its already-negotiated capabilities) is always reused as-is: this
    /// is the only place a new process is spawned, and it never resends Reserve or
    /// Start on its own -- it only ever returns a client that the caller then uses for
    /// whichever single typed operation it invoked.
    /// </summary>
    private DualHardwareCameraAgentOperations EnsureProcessStarted()
    {
        if (_process is { HasExited: false } && _pipeName is not null && _wireOperations is not null)
        {
            return _wireOperations;
        }

        DisposeExitedProcess();
        if (!AgentExecutableAvailable)
        {
            throw new HardwareCameraAgentLaunchException(
                $"Dual Camera Agent が見つかりません: {_agentExecutablePath}");
        }

        // Native requires --approved-capture-profile and --dual-identity-proof to
        // already be existing regular files, and exits 1 immediately if either is
        // missing (docs/HARDWARE_CAMERA_AGENT_DUAL_V2.md, Issue #22). Both are
        // approval/proof artifacts -- an approved capture profile and a verified
        // identity proof -- so this class never fabricates their contents. A missing
        // file must stop here with zero process and zero pipe connections, not spawn
        // a Native process that is guaranteed to fail closed on its own moments later.
        EnsureRequiredArtifactFileExists(_approvedCaptureProfilePath, "承認済みcapture profile");
        EnsureRequiredArtifactFileExists(_dualIdentityProofPath, "dual identity proof");
        if (_wpdCameraMapPath is not null)
        {
            EnsureRequiredArtifactFileExists(_wpdCameraMapPath, "WPD camera map");
        }

        // Validate the local path chain (fixed drive, no reparse point/junction) before
        // ever creating anything through it -- Directory.CreateDirectory silently
        // follows an existing junction, so checking only afterward would let a planted
        // reparse point redirect the create before the guard ever gets a chance to run.
        // (The capture-profile/identity-proof directories need no separate guard-then-
        // create pair: EnsureRequiredArtifactFileExists above already proved each file,
        // and therefore its parent directory, exists safely.)
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_pairJournalRootPath);
        Directory.CreateDirectory(_pairJournalRootPath);

        // Hard constraint (Issue #5 review / Issue #8 comment): a fresh, unique pipe
        // name every launch, in the same pattern as Single v2. Never connect to a
        // fixed/default name -- that risks a fail-open connection to a stale host.
        var pipeName = $"{DualHardwareCameraAgentProtocol.DefaultPipeName}.{Guid.NewGuid():N}";
        var bindingPipeName = _wpdCameraMapPath is null
            ? null
            : $"{DualBindingCameraAgentProtocol.DefaultPipeName}.{Guid.NewGuid():N}";
        var process = new Process
        {
            StartInfo = CreateStartInfo(pipeName, bindingPipeName),
            EnableRaisingEvents = true,
        };
        try
        {
            if (!process.Start())
            {
                throw new HardwareCameraAgentLaunchException("Dual Camera Agent を開始できませんでした。");
            }
            Interlocked.Increment(ref _processGeneration);
            _process = process;
            _standardOutput = process.StandardOutput.ReadToEndAsync(CancellationToken.None);
            _standardError = process.StandardError.ReadToEndAsync(CancellationToken.None);
            _pipeName = pipeName;
            _bindingPipeName = bindingPipeName;
            _wireOperations = new DualHardwareCameraAgentOperations(
                pipeName, process.Id, ConnectTimeout, ResponseTimeout);
            _bindingWireTransport = bindingPipeName is null
                ? null
                : new NamedPipeHardwareCameraAgentTransport(
                    bindingPipeName, process.Id, ConnectTimeout, ResponseTimeout);
            _bindingSessionMayNeedCleanup = false;
            _bindingCancellationResponseReceived = false;
            _bindingCancellationSucceeded = false;
            _captureHostActivated = false;
            LastObservedAgentExitCode = null;
            return _wireOperations;
        }
        catch (HardwareCameraAgentLaunchException)
        {
            process.Dispose();
            _pipeName = null;
            _bindingPipeName = null;
            _wireOperations = null;
            _bindingWireTransport = null;
            _bindingSessionMayNeedCleanup = false;
            _bindingCancellationResponseReceived = false;
            _bindingCancellationSucceeded = false;
            _captureHostActivated = false;
            throw;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            process.Dispose();
            _pipeName = null;
            _bindingPipeName = null;
            _wireOperations = null;
            _bindingWireTransport = null;
            _bindingSessionMayNeedCleanup = false;
            _bindingCancellationResponseReceived = false;
            _bindingCancellationSucceeded = false;
            _captureHostActivated = false;
            throw new HardwareCameraAgentLaunchException("Dual Camera Agent の開始に失敗しました。", exception);
        }
    }

    private IHardwareCameraAgentTransport EnsureBindingProcessStarted()
    {
        _ = EnsureProcessStarted();
        return _bindingWireTransport ?? throw new HardwareCameraAgentLaunchException(
            "Dual binding requires a binding pipe and WPD camera map.",
            requestMayHaveBeenDispatched: false);
    }

    private ProcessStartInfo CreateStartInfo(string pipeName, string? bindingPipeName)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = _agentExecutablePath,
            WorkingDirectory = Path.GetDirectoryName(_agentExecutablePath)!,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        // Persistent (no --serve-once): one process serves the reserve / start /
        // query sequence for potentially several transactions across its lifetime,
        // matching the native shared-pair-journal, up-to-600s host contract.
        startInfo.ArgumentList.Add("--pipe-name");
        startInfo.ArgumentList.Add(pipeName);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_pairJournalRootPath);
        startInfo.ArgumentList.Add("--pair-journal-root");
        startInfo.ArgumentList.Add(_pairJournalRootPath);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(
            Path.GetDirectoryName(_approvedCaptureProfilePath)!);
        startInfo.ArgumentList.Add("--approved-capture-profile");
        startInfo.ArgumentList.Add(_approvedCaptureProfilePath);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(
            Path.GetDirectoryName(_dualIdentityProofPath)!);
        startInfo.ArgumentList.Add("--dual-identity-proof");
        startInfo.ArgumentList.Add(_dualIdentityProofPath);
        if (bindingPipeName is not null && _wpdCameraMapPath is not null)
        {
            EnsureRequiredArtifactFileExists(_wpdCameraMapPath, "WPD camera map");
            startInfo.ArgumentList.Add("--binding-pipe-name");
            startInfo.ArgumentList.Add(bindingPipeName);
            startInfo.ArgumentList.Add("--wpd-camera-map");
            startInfo.ArgumentList.Add(_wpdCameraMapPath);
        }
        return startInfo;
    }

    private void RecordBindingLifecycleResponse(
        string operation,
        string requestJson,
        string responseJson)
    {
        using var request = JsonDocument.Parse(requestJson);
        var root = request.RootElement;
        var requestId = root.GetProperty("requestId").GetString()
            ?? throw new InvalidDataException("Dual binding requestId is missing.");

        if (operation == DualBindingCameraAgentProtocol.Operations.BeginBinding)
        {
            var beginReply = DualBindingCameraAgentProtocolCodec.DeserializeBeginBindingResponse(
                responseJson,
                requestId);
            _bindingSessionMayNeedCleanup = beginReply.Succeeded;
            _bindingCancellationResponseReceived = false;
            _bindingCancellationSucceeded = false;
            return;
        }

        if (operation is not (
                DualBindingCameraAgentProtocol.Operations.ActivateCapture or
                DualBindingCameraAgentProtocol.Operations.CancelBinding))
        {
            return;
        }

        var sessionId = root.GetProperty("payload").GetProperty("sessionId").GetString()
            ?? throw new InvalidDataException("Dual binding sessionId is missing.");
        if (operation == DualBindingCameraAgentProtocol.Operations.ActivateCapture)
        {
            var activationReply = DualBindingCameraAgentProtocolCodec.DeserializeActivateCaptureResponse(
                responseJson,
                requestId,
                sessionId);
            if (activationReply.Succeeded)
            {
                // The same Native process now owns the capture pipe. Binding
                // cancellation is no longer valid and capture recovery owns its
                // lifetime instead.
                _bindingSessionMayNeedCleanup = false;
                _captureHostActivated = true;
            }
            return;
        }

        var cancellationReply = DualBindingCameraAgentProtocolCodec.DeserializeCancelBindingResponse(
            responseJson,
            requestId,
            sessionId);
        _bindingCancellationResponseReceived = true;
        _bindingCancellationSucceeded = cancellationReply.Succeeded;
    }

    private static string ReadBindingOperation(string requestJson)
    {
        using var request = JsonDocument.Parse(requestJson);
        var root = request.RootElement;
        if (root.ValueKind != JsonValueKind.Object ||
            !root.TryGetProperty("operation", out var operation) ||
            operation.ValueKind != JsonValueKind.String ||
            operation.GetString() is not { Length: > 0 } operationName)
        {
            throw new InvalidDataException("Dual binding operation is missing.");
        }
        return operationName;
    }

    private async Task<HardwareCameraAgentLaunchException> CreateConnectFailureAsync(
        HardwareCameraAgentConnectException cause)
    {
        var (exitCode, sanitized) = await CaptureExitDiagnosticsAsync().ConfigureAwait(false);
        var exitSummary = exitCode.HasValue ? exitCode.Value.ToString() : "unavailable";
        var stderrSummary = string.IsNullOrEmpty(sanitized) ? "unavailable" : sanitized;

        // A connect failure means the pipe was never reached, so -- unlike an
        // incomplete response -- this is never ambiguous: the request was definitely
        // never dispatched, regardless of what exit code (if any) the process shows.
        return new HardwareCameraAgentLaunchException(
            $"Dual Camera Agent へ接続できませんでした。ExitCode={exitSummary}; stderr={stderrSummary}",
            cause,
            requestMayHaveBeenDispatched: false,
            processExitCode: exitCode,
            sanitizedStandardError: sanitized);
    }

    private async Task<HardwareCameraAgentLaunchException> CreatePipeFailureAsync(IOException cause)
    {
        var (exitCode, sanitized) = await CaptureExitDiagnosticsAsync().ConfigureAwait(false);
        var responseFailureStage = (cause as HardwareCameraAgentIncompleteResponseException)?.FailureStage;

        // Exit code contract (Native pipe protocol, Issue #5 -- unchanged):
        //   0 = complete, including a natural max-lifetime exit
        //   1 = argument/launch failure -> never reached the pipe, not dispatched
        //   2 = failed_before_dispatch -> explicitly known not dispatched
        //   3 = dispatched_delivery_failed -> ambiguous, may have been dispatched
        // Any other exit code, or a still-unresolved process, is treated as
        // ambiguous and fails closed: after start-reserved-pair's write completes,
        // every subsequent transport failure may mean the pair was captured.
        var requestMayHaveBeenDispatched = exitCode is not (1 or 2);
        var exitSummary = exitCode.HasValue ? exitCode.Value.ToString() : "unavailable";
        var responseStageSummary = responseFailureStage?.ToString() ?? "unavailable";
        var stderrSummary = string.IsNullOrEmpty(sanitized) ? "unavailable" : sanitized;
        return new HardwareCameraAgentLaunchException(
            $"Dual Camera Agent pipe response was incomplete. Stage={responseStageSummary}; " +
            $"ExitCode={exitSummary}; stderr={stderrSummary}",
            cause,
            requestMayHaveBeenDispatched: requestMayHaveBeenDispatched,
            processExitCode: exitCode,
            sanitizedStandardError: sanitized,
            responseFailureStage: responseFailureStage);
    }

    /// <summary>
    /// Waits briefly for the current process to reach a stable exited state and, if
    /// it has, returns its exit code and sanitized stderr. Shared by both the connect-
    /// failure and incomplete-response diagnostic paths so a Native process that
    /// failed fast (e.g. bad arguments, a missing required artifact file) always
    /// surfaces its exit code and reason instead of only a generic transport error.
    /// </summary>
    private async Task<(int? ExitCode, string SanitizedStandardError)> CaptureExitDiagnosticsAsync()
    {
        int? exitCode = null;
        var standardError = string.Empty;
        if (_process is not null)
        {
            try
            {
                if (!_process.HasExited)
                {
                    await _process.WaitForExitAsync(CancellationToken.None)
                        .WaitAsync(TimeSpan.FromSeconds(2))
                        .ConfigureAwait(false);
                }
                if (_process.HasExited)
                {
                    exitCode = _process.ExitCode;
                    if (_standardError is not null)
                    {
                        standardError = await _standardError
                            .WaitAsync(TimeSpan.FromSeconds(1))
                            .ConfigureAwait(false);
                    }
                }
            }
            catch (Exception exception) when (
                exception is InvalidOperationException or TimeoutException or System.ComponentModel.Win32Exception)
            {
                // The underlying transport failure remains authoritative when the
                // child has not reached a stable exited state within the diagnostic
                // bound.
            }
        }

        return (exitCode, HardwareCameraAgentDiagnostic.SanitizeStandardError(standardError));
    }

    /// <summary>
    /// Fails closed before any process launch if a required approval/proof artifact
    /// is missing or is not a regular file. Never creates or fabricates the file --
    /// only a real approval workflow may produce one.
    /// </summary>
    private static void EnsureRequiredArtifactFileExists(string path, string description)
    {
        try
        {
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
        }
        catch (InvalidDataException exception)
        {
            throw new HardwareCameraAgentLaunchException(
                $"{description}のパスを安全に検証できません(生成しません): {path}",
                exception,
                requestMayHaveBeenDispatched: false);
        }
        if (!File.Exists(path))
        {
            throw new HardwareCameraAgentLaunchException(
                $"{description}が見つかりません。Native は既存の通常ファイルを要求します(自動生成はしません): {path}",
                requestMayHaveBeenDispatched: false);
        }
        var attributes = File.GetAttributes(path);
        if ((attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new HardwareCameraAgentLaunchException(
                $"{description}は通常ファイルではありません: {path}",
                requestMayHaveBeenDispatched: false);
        }
    }

    private void DisposeExitedProcess()
    {
        if (_process is null)
        {
            return;
        }
        if (!_process.HasExited)
        {
            // A live process is reused as-is; EnsureProcessStarted only reaches this
            // point once the cached process/pipe/wire operations are already stale.
            return;
        }
        _process.Dispose();
        _process = null;
        _standardOutput = null;
        _standardError = null;
        _pipeName = null;
        _bindingPipeName = null;
        _wireOperations = null;
        _bindingWireTransport = null;
        _bindingSessionMayNeedCleanup = false;
        _bindingCancellationResponseReceived = false;
        _bindingCancellationSucceeded = false;
        _captureHostActivated = false;
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return;
        }
        await _operationGate.WaitAsync().ConfigureAwait(false);
        var shutdownCompleted = false;
        try
        {
            Exception? shutdownFailure = null;
            if (_captureHostActivated)
            {
                shutdownFailure = await WaitForActivatedCaptureHostExitAsync().ConfigureAwait(false);
            }
            else if (_bindingSessionMayNeedCleanup)
            {
                if (!_bindingCancellationResponseReceived)
                {
                    shutdownFailure = new HardwareCameraAgentLaunchException(
                        "Dual binding cleanup was not acknowledged; the exclusive hardware lease must remain held.",
                        requestMayHaveBeenDispatched: false);
                }
                else if (!_bindingCancellationSucceeded)
                {
                    shutdownFailure = new HardwareCameraAgentLaunchException(
                        "Dual binding cleanup was refused; the exclusive hardware lease must remain held.",
                        requestMayHaveBeenDispatched: false,
                        processExitCode: _process is { HasExited: true } failedProcess
                            ? failedProcess.ExitCode
                            : null);
                }
                else if (_process is not { } process)
                {
                    shutdownFailure = new HardwareCameraAgentLaunchException(
                        "Dual binding cleanup was acknowledged, but the Agent process cannot be verified as exited.",
                        requestMayHaveBeenDispatched: false);
                }
                else
                {
                    try
                    {
                        if (!process.HasExited)
                        {
                            await process.WaitForExitAsync(CancellationToken.None)
                                .WaitAsync(BindingShutdownExitTimeout)
                                .ConfigureAwait(false);
                        }
                        if (process.ExitCode != 0)
                        {
                            shutdownFailure = new HardwareCameraAgentLaunchException(
                                $"Dual binding cleanup was acknowledged but Agent exited with code {process.ExitCode}.",
                                requestMayHaveBeenDispatched: false,
                                processExitCode: process.ExitCode);
                        }
                        else
                        {
                            _bindingSessionMayNeedCleanup = false;
                        }
                    }
                    catch (TimeoutException exception)
                    {
                        shutdownFailure = new HardwareCameraAgentLaunchException(
                            "Dual binding cleanup response was delivered, but Agent did not exit within the bounded wait.",
                            exception,
                            requestMayHaveBeenDispatched: false);
                    }
                }
            }

            if (shutdownFailure is not null)
            {
                // Do not dispose the Process handle or semaphore: the caller keeps
                // its window and exclusive hardware lease alive in a Blocking state.
                // A later operator close is an explicit attempt, never an automatic
                // retry; force-kill remains prohibited.
                throw shutdownFailure;
            }

            // Never kill a Dual Camera Agent process. Once a pair transaction may
            // have been dispatched, only the native max-lifetime and durable pair
            // journal own its resolution; disposing the .NET Process handle here
            // does not send any kill signal to the still-running native process.
            _process?.Dispose();
            _process = null;
            _pipeName = null;
            _bindingPipeName = null;
            _wireOperations = null;
            _bindingWireTransport = null;
            _bindingSessionMayNeedCleanup = false;
            _bindingCancellationResponseReceived = false;
            _bindingCancellationSucceeded = false;
            _captureHostActivated = false;
            _disposed = true;
            shutdownCompleted = true;
        }
        finally
        {
            _operationGate.Release();
            if (shutdownCompleted)
            {
                _operationGate.Dispose();
            }
        }
    }

    private async Task<HardwareCameraAgentLaunchException?> WaitForActivatedCaptureHostExitAsync()
    {
        if (_process is not { } process)
        {
            return new HardwareCameraAgentLaunchException(
                "Activated capture host cannot be verified for natural exit; the exclusive hardware lease must remain held.",
                requestMayHaveBeenDispatched: true);
        }

        try
        {
            if (!process.HasExited)
            {
                await process.WaitForExitAsync(CancellationToken.None)
                    .WaitAsync(BindingShutdownExitTimeout)
                    .ConfigureAwait(false);
            }
        }
        catch (TimeoutException exception)
        {
            return new HardwareCameraAgentLaunchException(
                "Activated capture host did not exit within the bounded wait; the exclusive hardware lease must remain held.",
                exception,
                requestMayHaveBeenDispatched: true);
        }

        try
        {
            if (!process.HasExited)
            {
                return new HardwareCameraAgentLaunchException(
                    "Activated capture host exit could not be confirmed; the exclusive hardware lease must remain held.",
                    requestMayHaveBeenDispatched: true);
            }

            // An observed non-zero exit is not disguised as success in diagnostics,
            // but the process has ended naturally, so retaining the physical lease
            // would no longer protect any live hardware session.
            LastObservedAgentExitCode = process.ExitCode;
            return null;
        }
        catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            return new HardwareCameraAgentLaunchException(
                "Activated capture host exit could not be inspected; the exclusive hardware lease must remain held.",
                exception,
                requestMayHaveBeenDispatched: true);
        }
    }

    private static bool IsUncPath(string path) =>
        path.StartsWith("\\\\", StringComparison.Ordinal) ||
        path.StartsWith("//", StringComparison.Ordinal);
}
