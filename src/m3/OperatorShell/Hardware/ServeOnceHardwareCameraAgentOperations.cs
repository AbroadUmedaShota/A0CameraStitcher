using System.Diagnostics;
using System.IO;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

public sealed class HardwareCameraAgentLaunchException : Exception
{
    public HardwareCameraAgentLaunchException(
        string message,
        Exception? innerException = null,
        bool requestMayHaveBeenDispatched = false)
        : base(message, innerException)
    {
        RequestMayHaveBeenDispatched = requestMayHaveBeenDispatched;
    }

    public bool RequestMayHaveBeenDispatched { get; }
}

public sealed class ServeOnceHardwareCameraAgentOperations : IHardwareSingleCameraOperations
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan QueryResponseTimeout = TimeSpan.FromSeconds(45);
    private static readonly TimeSpan LiveViewResponseTimeout = TimeSpan.FromSeconds(90);
    private static readonly TimeSpan CaptureResponseTimeout = TimeSpan.FromSeconds(240);
    private readonly string _agentExecutablePath;

    public ServeOnceHardwareCameraAgentOperations(string agentExecutablePath)
    {
        if (string.IsNullOrWhiteSpace(agentExecutablePath))
        {
            throw new ArgumentException("A Camera Agent executable path is required.", nameof(agentExecutablePath));
        }

        _agentExecutablePath = Path.GetFullPath(agentExecutablePath);
    }

    public string AgentExecutablePath => _agentExecutablePath;

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
                exception is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
            {
                return false;
            }
        }
    }

    public Task<HardwareCameraAgentReply<HardwareSingleReadinessResult>> GetReadinessAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default) =>
        RunAsync(
            QueryResponseTimeout,
            (client, token) => client.GetSingleReadinessAsync(cameraAlias, token),
            cancellationToken);

    public Task<HardwareCameraAgentReply<HardwareSingleLiveViewResult>> ProbeLiveViewAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default) =>
        RunAsync(
            LiveViewResponseTimeout,
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
        // Build the exact wire request once, before creating a child. In
        // particular, do not re-run the time-sensitive profile-expiry
        // validation after the child starts: a failure there would be known
        // undispatched but could otherwise be mistaken for an ambiguous
        // post-dispatch failure.
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

        return RunPipeAsync(
            CaptureResponseTimeout,
            async (pipeName, token) =>
            {
                var transport = new NamedPipeHardwareCameraAgentTransport(
                    pipeName,
                    ConnectTimeout,
                    CaptureResponseTimeout);
                var responseJson = await transport.SendAsync(requestJson, token).ConfigureAwait(false);
                return HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
                    responseJson,
                    request.RequestId,
                    transactionId,
                    cameraAlias,
                    expectedProfile,
                    liveViewHandoffRequested);
            },
            cancellationToken);
    }

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool expectedLiveViewHandoffRequested,
        CancellationToken cancellationToken = default) =>
        RunAsync(
            QueryResponseTimeout,
            (client, token) => client.GetTransactionResultAsync(
                transactionId,
                expectedCameraAlias,
                expectedProfile,
                expectedLiveViewHandoffRequested,
                token),
            cancellationToken);

    private async Task<T> RunAsync<T>(
        TimeSpan responseTimeout,
        Func<HardwareCameraAgentClient, CancellationToken, Task<T>> operation,
        CancellationToken cancellationToken) =>
        await RunPipeAsync(
            responseTimeout,
            (pipeName, token) => operation(
                new HardwareCameraAgentClient(pipeName, ConnectTimeout, responseTimeout),
                token),
            cancellationToken).ConfigureAwait(false);

    private async Task<T> RunPipeAsync<T>(
        TimeSpan responseTimeout,
        Func<string, CancellationToken, Task<T>> operation,
        CancellationToken cancellationToken)
    {
        if (!AgentExecutableAvailable)
        {
            throw new HardwareCameraAgentLaunchException(
                $"Camera Agent が見つかりません: {_agentExecutablePath}");
        }

        var pipeName = $"A0CameraStitcher.CameraAgent.Hardware.v1.{Guid.NewGuid():N}";
        using var process = new Process
        {
            StartInfo = CreateStartInfo(pipeName),
            EnableRaisingEvents = true,
        };

        try
        {
            if (!process.Start())
            {
                throw new HardwareCameraAgentLaunchException("Camera Agent を開始できませんでした。");
            }
        }
        catch (HardwareCameraAgentLaunchException)
        {
            throw;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            throw new HardwareCameraAgentLaunchException("Camera Agent の開始に失敗しました。", exception);
        }

        var standardOutput = process.StandardOutput.ReadToEndAsync(CancellationToken.None);
        var standardError = process.StandardError.ReadToEndAsync(CancellationToken.None);
        try
        {
            var result = await operation(pipeName, cancellationToken).ConfigureAwait(false);
            await WaitForSuccessfulExitAsync(process, standardOutput, standardError).ConfigureAwait(false);
            return result;
        }
        catch (HardwareCameraAgentConnectException)
        {
            // This typed exception is emitted only before a request frame was
            // dispatched. The exact child we just spawned therefore cannot be
            // in camera work and may be terminated to avoid an orphaned
            // serve-once ConnectNamedPipe waiter.
            TerminateUndispatchedAgent(process);
            throw;
        }
        catch
        {
            // Never terminate a spawned agent here. Once a request has reached
            // capture-single, the agent owns the transaction through its durable
            // terminal journal even if the WPF process or pipe disconnects.
            throw;
        }
    }

    private static void TerminateUndispatchedAgent(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: false);
                if (!process.WaitForExit(milliseconds: 5000))
                {
                    throw new HardwareCameraAgentLaunchException(
                        "未dispatchのCamera Agentを安全時間内に終了できませんでした。");
                }
            }
        }
        catch (HardwareCameraAgentLaunchException)
        {
            throw;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            throw new HardwareCameraAgentLaunchException(
                "未dispatchのCamera Agentを終了できませんでした。カメラ要求は送信していません。",
                exception);
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
        startInfo.ArgumentList.Add("--serve-once");
        startInfo.ArgumentList.Add("--pipe-name");
        startInfo.ArgumentList.Add(pipeName);
        return startInfo;
    }

    private static async Task WaitForSuccessfulExitAsync(
        Process process,
        Task<string> standardOutput,
        Task<string> standardError)
    {
        try
        {
            await process.WaitForExitAsync(CancellationToken.None)
                .WaitAsync(TimeSpan.FromSeconds(10))
                .ConfigureAwait(false);
        }
        catch (TimeoutException exception)
        {
            throw new HardwareCameraAgentLaunchException(
                "Camera Agent は応答後に安全終了しませんでした。プロセスは強制終了していません。",
                exception,
                requestMayHaveBeenDispatched: true);
        }

        var output = await standardOutput.ConfigureAwait(false);
        var error = await standardError.ConfigureAwait(false);
        if (process.ExitCode != 0)
        {
            var detail = SanitizeProcessDetail(string.IsNullOrWhiteSpace(error) ? output : error);
            throw new HardwareCameraAgentLaunchException(
                string.IsNullOrEmpty(detail)
                    ? $"Camera Agent が終了コード {process.ExitCode} で終了しました。"
                    : $"Camera Agent が終了コード {process.ExitCode} で終了しました: {detail}",
                requestMayHaveBeenDispatched: true);
        }
    }

    private static string SanitizeProcessDetail(string value)
    {
        var singleLine = string.Join(
            ' ',
            value.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
        return singleLine.Length <= 300 ? singleLine : singleLine[..300];
    }

    private static bool IsUncPath(string path) =>
        path.StartsWith("\\\\", StringComparison.Ordinal) ||
        path.StartsWith("//", StringComparison.Ordinal);
}
