using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

public interface IHardwareContinuousLiveViewOperations
{
    string CreateSessionId();

    Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> StartLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> ReadLiveViewFrameAsync(
        string sessionId,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> HeartbeatLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default);

    Task<HardwareCameraAgentReply<HardwareContinuousLiveViewResult>> StopLiveViewAsync(
        string sessionId,
        CancellationToken cancellationToken = default);
}
