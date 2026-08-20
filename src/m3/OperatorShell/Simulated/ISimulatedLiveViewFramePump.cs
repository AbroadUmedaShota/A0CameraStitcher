namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// Periodically raises a <see cref="SimulatedLiveViewFrameTick"/> for one camera alias at a
/// time, off the UI thread. Subscribers are responsible for marshalling <see cref="Tick"/> to
/// their own thread (see OperatorShellViewModel's SynchronizationContext.Post usage) and for
/// turning the tick into an actual rendered frame via <see cref="ISimulatedLiveViewFrameSource"/>.
/// Implementations must raise ticks only while a camera alias is active; nothing is raised
/// before <see cref="Start"/> or after <see cref="Stop"/>, keeping the "no frame supply while
/// Live View is OFF" contract enforceable by the caller.
/// </summary>
public interface ISimulatedLiveViewFramePump : IDisposable
{
    event EventHandler<SimulatedLiveViewFrameTick>? Tick;

    /// <summary>Starts (or restarts) the pump for <paramref name="cameraAlias"/> and returns
    /// the new generation number. Callers should remember this value and reject any tick
    /// whose <see cref="SimulatedLiveViewFrameTick.Generation"/> does not match it, so a tick
    /// from a since-superseded session (e.g. Stop() then Start() again for the same alias)
    /// cannot be mistaken for a current one.</summary>
    int Start(string cameraAlias, SimulatedFramePattern pattern);

    void Stop();

    void SetPattern(SimulatedFramePattern pattern);
}
