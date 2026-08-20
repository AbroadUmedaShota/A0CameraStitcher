using System.Windows.Media.Imaging;

namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// One rendered SIMULATED live view frame. Preview-only: this type and everything that
/// produces it must never be wired into original-image or stitching input (the LV
/// non-input contract documented in docs/M3_SIMULATED_FOUNDATION.md applies here too).
/// </summary>
public sealed record SimulatedLiveViewFrame
{
    public required string CameraAlias { get; init; }

    public required SimulatedFramePattern Pattern { get; init; }

    public required int SequenceNumber { get; init; }

    public required DateTimeOffset CapturedAtUtc { get; init; }

    public required BitmapSource Image { get; init; }

    /// <summary>Always true. Mirrors the simulated/marker contract used by the IPC protocol
    /// so downstream consumers can apply the same defensive check.</summary>
    public required bool Simulation { get; init; }

    /// <summary>Always "Simulated". See <see cref="Simulation"/>.</summary>
    public required string Marker { get; init; }
}
