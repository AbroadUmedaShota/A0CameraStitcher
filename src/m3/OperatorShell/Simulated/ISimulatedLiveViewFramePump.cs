namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// Periodically produces SIMULATED live view frames for one camera alias at a time, off the
/// UI thread. Subscribers are responsible for marshalling <see cref="FrameProduced"/> to
/// their own thread (see OperatorShellViewModel's SynchronizationContext.Post usage).
/// Implementations must supply frames only while a camera alias is active; nothing is
/// produced before <see cref="Start"/> or after <see cref="Stop"/>, keeping the "no frame
/// supply while Live View is OFF" contract enforceable by the caller.
/// </summary>
public interface ISimulatedLiveViewFramePump : IDisposable
{
    event EventHandler<SimulatedLiveViewFrame>? FrameProduced;

    void Start(string cameraAlias, SimulatedFramePattern pattern);

    void Stop();

    void SetPattern(SimulatedFramePattern pattern);
}
