using System.Diagnostics;
using System.IO;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

public sealed class PersistentHardwareCameraAgentOperations :
    IHardwareSingleCameraOperations,
    IHardwareContinuousLiveViewOperations,
    IAsyncDisposable
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan QueryResponseTimeout = TimeSpan.FromSeconds(45);
    private static readonly TimeSpan LiveViewResponseTimeout = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan CaptureResponseTimeout = TimeSpan.FromSeconds(240);
    private readonly string _agentExecutablePath;
    private readonly string _captureProfilePath;
    private readonly string _singleIdentityV3Path;
    private readonly SemaphoreSlim _operationGate = new(1, 1);
    private Process? _process;
    private Task<string>? _standardOutput;
    private Task<string>? _standardError;
    private string? _pipeName;
    private string? _ownedSessionId;
    private bool _captureMayBeActive;
    private bool _disposed;

    public PersistentHardwareCameraAgentOperations(
        string agentExecutablePath,
        string? captureProfilePath = null,
        string? singleIdentityV3Path = null)
    {
        if (string.IsNullOrWhiteSpace(agentExecutablePath))
        {
            throw new ArgumentException("A Camera Agent executable path is required.", nameof(agentExecutablePath));
        }

        _agentExecutablePath = Path.GetFullPath(agentExecutablePath);
        _captureProfilePath = string.IsNullOrWhiteSpace(captureProfilePath)
            ? string.Empty
            : Path.GetFullPath(captureProfilePath);
        _singleIdentityV3Path = string.IsNullOrWhiteSpace(singleIdentityV3Path)
            ? string.Empty
            : Path.GetFullPath(singleIdentityV3Path);
    }

    public string AgentExecutablePath => _agentExecutablePath;

    public bool AgentExecutableAvailable
    {
        get
        {
            try
            {
                if (!Path.IsPathFullyQualified(_agentExecutablePath) ||
                    _agentExecutablePath.StartsWith("\\\\", StringComparison.Ordinal) ||
                    _agentExecutablePath.StartsWith("//", StringComparison.Ordinal) ||
                    !File.Exists(_agentExecutablePath))
                {
                    return false;
                }

                WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(_agentExecutablePath);
                var attributes = File.GetAttributes(_agentExecutablePath);
                return (attributes & (FileAttributes.Directory | FileAttributes.ReparsePoint)) == 0;
            }
            catch (Exception exception) when (
                exception is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
            {
                return false;
            }
        }
    }

    public Task<HardwareCameraAgentReply<HardwareSingleReadinessResult>> GetReadinessAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default) =>
        RunV1Async(
            QueryResponseTimeout,
            (client, token) => client.GetSingleReadinessAsync(cameraAlias, token),
            cancellationToken);

    public Task<HardwareCameraAgentReply<HardwareSingleLiveViewResult>> ProbeLiveViewAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default) =>
        RunV1Async(
            TimeSpan.FromSeconds(90),
            (client, token) => client.ProbeLiveViewAsync(
                cameraAlias,
                HardwareLiveViewSafetyConfirmation.Confirmed,
                frames: 1,
                intervalMs: 100,
                token),
            cancellationToken);

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> CaptureAsync(
        string transactionId,
        string cameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool liveViewHandoffRequested,
        CancellationToken cancellationToken = default)
    {
        HardwareCameraAgentRequestEnvelope request;
        string requestJson;
        try
        {
            request = HardwareCameraAgentProtocolCodec.CreateCaptureRequest(
                transactionId,
                cameraAlias,
                expectedProfile,
                HardwareCaptureSafetyConfirmations.AllConfirmed,
                liveViewHandoffRequested);
            requestJson = HardwareCameraAgentProtocolCodec.SerializeRequest(request);
        }
        catch (Exception exception) when (
            exception is HardwareProtocolViolationException or ArgumentException)
        {
            throw new HardwareCameraAgentLaunchException(
                "Camera Agent起動前にcapture request validationが失敗しました。",
                exception,
                requestMayHaveBeenDispatched: false);
        }

        return RunSerializedAsync(
            CaptureResponseTimeout,
            async (pipeName, token) =>
            {
                var transport = new NamedPipeHardwareCameraAgentTransport(
                    pipeName, ConnectTimeout, CaptureResponseTimeout);
                _captureMayBeActive = true;
                try
                {
                    var responseJson = await transport.SendAsync(requestJson, token).ConfigureAwait(false);
                    var reply = HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
                        responseJson,
                        request.RequestId,
                        transactionId,
                        cameraAlias,
                        expectedProfile,
                        liveViewHandoffRequested);
                    _captureMayBeActive = reply.Payload.TerminalState is "Reserved" or "InProgress";
                    return reply;
                }
                catch (HardwareCameraAgentConnectException)
                {
                    _captureMayBeActive = false;
                    throw;
                }
            },
            cancellationToken);
    }

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool expectedLiveViewHandoffRequested,
        CancellationToken cancellationToken = default) =>
        RunV1Async(
            QueryResponseTimeout,
            async (client, token) =>
            {
                var reply = await client.GetTransactionResultAsync(
                    transactionId,
                    expectedCameraAlias,
                    expectedProfile,
                    expectedLiveViewHandoffRequested,
                    token).ConfigureAwait(false);
                _captureMayBeActive = reply.Payload.TerminalState is "Reserved" or "InProgress";
                return reply;
            },
            cancellationToken,
            allowWhileCaptureMayBeActive: true);

    public string CreateSessionId() => HardwareContinuousLiveViewClient.CreateSessionId();

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> StartLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        RunV2Async(
            (client, token) => client.StartAsync(sessionId, token),
            sessionId,
            setOwnedSession: true,
            cancellationToken);

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> ReadLiveViewFrameAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        RunV2Async((client, token) => client.ReadFrameAsync(sessionId, token), sessionId, false, cancellationToken);

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> HeartbeatLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        RunV2Async((client, token) => client.HeartbeatAsync(sessionId, token), sessionId, false, cancellationToken);

    public Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> StopLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default) =>
        RunV2Async(
            async (client, token) =>
            {
                var reply = await client.StopAsync(sessionId, token).ConfigureAwait(false);
                if (reply.Success)
                {
                    _ownedSessionId = sessionId;
                }
                return reply;
            },
            sessionId,
            false,
            cancellationToken);

    private Task<T> RunV1Async<T>(
        TimeSpan responseTimeout,
        Func<HardwareCameraAgentClient, CancellationToken, Task<T>> operation,
        CancellationToken cancellationToken,
        bool allowWhileCaptureMayBeActive = false) =>
        RunSerializedAsync(
            responseTimeout,
            (pipeName, token) => operation(
                new HardwareCameraAgentClient(pipeName, ConnectTimeout, responseTimeout),
                token),
            cancellationToken,
            allowWhileCaptureMayBeActive);

    private Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> RunV2Async(
        Func<HardwareContinuousLiveViewClient, CancellationToken,
            Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>>> operation,
        string sessionId,
        bool setOwnedSession,
        CancellationToken cancellationToken) =>
        RunSerializedAsync(
            LiveViewResponseTimeout,
            async (pipeName, token) =>
            {
                if (!setOwnedSession && _ownedSessionId is not null &&
                    _ownedSessionId != sessionId)
                {
                    throw new InvalidOperationException("A different Live View session owns the Camera Agent.");
                }
                var transport = new NamedPipeHardwareCameraAgentTransport(
                    pipeName, ConnectTimeout, LiveViewResponseTimeout);
                var reply = await operation(new HardwareContinuousLiveViewClient(transport), token)
                    .ConfigureAwait(false);
                if (setOwnedSession && reply.Success)
                {
                    _ownedSessionId = sessionId;
                }
                return reply;
            },
            cancellationToken);

    private async Task<T> RunSerializedAsync<T>(
        TimeSpan responseTimeout,
        Func<string, CancellationToken, Task<T>> operation,
        CancellationToken cancellationToken,
        bool allowWhileCaptureMayBeActive = false)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        await _operationGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var pipeName = EnsureProcessStarted(allowWhileCaptureMayBeActive);
            try
            {
                return await operation(pipeName, cancellationToken).ConfigureAwait(false);
            }
            catch (IOException exception) when (exception is not HardwareCameraAgentConnectException)
            {
                throw await CreatePipeFailureAsync(exception).ConfigureAwait(false);
            }
        }
        finally
        {
            _operationGate.Release();
        }
    }

    private async Task<HardwareCameraAgentLaunchException> CreatePipeFailureAsync(IOException cause)
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
                // The pipe failure remains authoritative when the child has not
                // reached a stable exited state within the diagnostic bound.
            }
        }

        var sanitized = HardwareCameraAgentDiagnostic.SanitizeStandardError(standardError);
        var responseFailureStage =
            (cause as HardwareCameraAgentIncompleteResponseException)?.FailureStage;
        var exitSummary = exitCode.HasValue ? exitCode.Value.ToString() : "unavailable";
        var responseStageSummary = responseFailureStage?.ToString() ?? "unavailable";
        var stderrSummary = string.IsNullOrEmpty(sanitized) ? "unavailable" : sanitized;
        return new HardwareCameraAgentLaunchException(
            $"Camera Agent pipe response was incomplete. Stage={responseStageSummary}; " +
            $"ExitCode={exitSummary}; stderr={stderrSummary}",
            cause,
            requestMayHaveBeenDispatched: true,
            processExitCode: exitCode,
            sanitizedStandardError: sanitized,
            responseFailureStage: responseFailureStage);
    }

    private string EnsureProcessStarted(bool allowWhileCaptureMayBeActive)
    {
        if (_process is { HasExited: false } && _pipeName is not null)
        {
            return _pipeName;
        }
        // GitHub Issue #93: capture が未確定(_captureMayBeActive)でも、結果を確認する
        // 読み取り専用の get-transaction-result だけは新しい agent を起動して照会できる
        // 必要がある(照会は durable journal を読むだけで capture を再実行しない)。これを
        // ブロックすると、capture 送信中に例外が出て agent プロセスも落ちた場合、アプリ
        // 再起動以外に pending transaction を解消する手段が無くなる。capture / Live View は
        // 従来どおりゲートする(allowWhileCaptureMayBeActive=false)。
        if (_captureMayBeActive && !allowWhileCaptureMayBeActive)
        {
            throw new HardwareCameraAgentLaunchException(
                "前のCamera Agent captureが未確定のため、新しいagent processを開始しません。",
                requestMayHaveBeenDispatched: true);
        }
        DisposeExitedProcess();
        if (!AgentExecutableAvailable)
        {
            throw new HardwareCameraAgentLaunchException(
                $"Camera Agent が見つかりません: {_agentExecutablePath}");
        }

        _pipeName = $"A0CameraStitcher.CameraAgent.Hardware.v2.{Guid.NewGuid():N}";
        var process = new Process
        {
            StartInfo = CreateStartInfo(_pipeName),
            EnableRaisingEvents = true,
        };
        try
        {
            if (!process.Start())
            {
                throw new HardwareCameraAgentLaunchException("Camera Agent を開始できませんでした。");
            }
            _process = process;
            _standardOutput = process.StandardOutput.ReadToEndAsync(CancellationToken.None);
            _standardError = process.StandardError.ReadToEndAsync(CancellationToken.None);
            return _pipeName;
        }
        catch (HardwareCameraAgentLaunchException)
        {
            process.Dispose();
            _pipeName = null;
            throw;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            process.Dispose();
            _pipeName = null;
            throw new HardwareCameraAgentLaunchException("Camera Agent の開始に失敗しました。", exception);
        }
    }

    private ProcessStartInfo CreateStartInfo(string pipeName)
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
        startInfo.ArgumentList.Add("--pipe-name");
        startInfo.ArgumentList.Add(pipeName);
        if (!string.IsNullOrEmpty(_captureProfilePath))
        {
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(
                Path.GetDirectoryName(_captureProfilePath)!);
            startInfo.ArgumentList.Add("--approved-capture-profile");
            startInfo.ArgumentList.Add(_captureProfilePath);
        }
        if (!string.IsNullOrEmpty(_singleIdentityV3Path))
        {
            WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(
                Path.GetDirectoryName(_singleIdentityV3Path)!);
            startInfo.ArgumentList.Add("--single-identity-v3");
            startInfo.ArgumentList.Add(_singleIdentityV3Path);
        }
        return startInfo;
    }

    private void DisposeExitedProcess()
    {
        if (_process is null)
        {
            return;
        }
        if (!_process.HasExited)
        {
            throw new InvalidOperationException("The active Camera Agent process cannot be replaced.");
        }
        _process.Dispose();
        _process = null;
        _standardOutput = null;
        _standardError = null;
        _pipeName = null;
        _ownedSessionId = null;
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        await _operationGate.WaitAsync().ConfigureAwait(false);
        try
        {
            if (_process is { HasExited: false } && _pipeName is not null && !_captureMayBeActive)
            {
                var sessionId = _ownedSessionId ?? HardwareContinuousLiveViewClient.CreateSessionId();
                try
                {
                    var transport = new NamedPipeHardwareCameraAgentTransport(
                        _pipeName, ConnectTimeout, LiveViewResponseTimeout);
                    _ = await new HardwareContinuousLiveViewClient(transport)
                        .CloseAsync(sessionId)
                        .ConfigureAwait(false);
                    await _process.WaitForExitAsync(CancellationToken.None)
                        .WaitAsync(TimeSpan.FromSeconds(10))
                        .ConfigureAwait(false);
                }
                catch (Exception exception) when (exception is not OutOfMemoryException)
                {
                    // Never kill a product Camera Agent. Its native heartbeat
                    // and maximum-lifetime guards own fail-closed cleanup.
                }
            }
            _process?.Dispose();
            _process = null;
        }
        finally
        {
            _operationGate.Release();
            _operationGate.Dispose();
        }
    }
}
