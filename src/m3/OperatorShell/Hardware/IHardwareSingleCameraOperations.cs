using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

public interface IHardwareSingleCameraOperations
{
    string AgentExecutablePath { get; }

    // The --artifacts-root value this same object supplies to the Camera
    // Agent child process (see PersistentHardwareCameraAgentOperations /
    // ServeOnceHardwareCameraAgentOperations). Callers verifying a retained
    // original or preview path must read it from here rather than
    // re-deriving it, so there is exactly one place that decides what root
    // the agent was actually launched with.
    string AgentArtifactsRoot { get; }

    bool AgentExecutableAvailable { get; }

    Task<HardwareCameraAgentReply<HardwareSingleReadinessResult>> GetReadinessAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareSingleLiveViewResult>> ProbeLiveViewAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> CaptureAsync(
        string transactionId,
        string cameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool liveViewHandoffRequested,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool expectedLiveViewHandoffRequested,
        CancellationToken cancellationToken = default);
}
