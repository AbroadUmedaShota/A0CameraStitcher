namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// A lightweight "it's time to render the next frame" notification raised by
/// <see cref="ISimulatedLiveViewFramePump"/>. It carries no image data: the pump only does
/// alias/pattern/sequence/generation bookkeeping on a thread-pool timer, and the actual WPF
/// rendering (<see cref="ISimulatedLiveViewFrameSource.CreateFrame"/>) is left to the
/// subscriber to run on whichever thread it chooses — in practice the UI thread, so every
/// <see cref="System.Windows.Media.Visual"/>/<see cref="System.Windows.Media.Imaging.RenderTargetBitmap"/>
/// created for a frame lives on the single Dispatcher the app already has, instead of one
/// being created per thread-pool thread that happens to run a tick.
/// </summary>
/// <param name="CameraAlias">The camera alias this tick was produced for.</param>
/// <param name="Pattern">The pattern active on the pump at tick time.</param>
/// <param name="SequenceNumber">Monotonically increasing per Start(); resets to 0 on each Start().</param>
/// <param name="Generation">The pump's Start() call count as of this tick, used to detect and
/// drop a tick from a superseded Live View session (e.g. OFF then back ON for the same
/// camera alias) even though the alias alone would look current.</param>
/// <param name="CapturedAtUtc">The timestamp to stamp on the rendered frame.</param>
public sealed record SimulatedLiveViewFrameTick(
    string CameraAlias,
    SimulatedFramePattern Pattern,
    int SequenceNumber,
    int Generation,
    DateTimeOffset CapturedAtUtc);
