namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// Renders SIMULATED live view test frames. Implementations must never read real camera
/// hardware, files, or network resources — every pixel is generated in code at call time.
/// </summary>
public interface ISimulatedLiveViewFrameSource
{
    SimulatedLiveViewFrame CreateFrame(
        string cameraAlias,
        SimulatedFramePattern pattern,
        int sequenceNumber,
        DateTimeOffset capturedAtUtc);
}
