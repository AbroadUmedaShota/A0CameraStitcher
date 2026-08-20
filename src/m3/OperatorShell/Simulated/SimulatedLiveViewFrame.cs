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

    /// <summary>The Gaussian blur radius actually rendered into this frame (0 = sharp).
    /// Always 0 for every pattern except <see cref="SimulatedFramePattern.BlurToFocusTransition"/>,
    /// where it follows that pattern's ramp-down-then-hold-sharp cycle. Not required (defaults
    /// to 0) so existing frame construction call sites — including the 1x1 test doubles used
    /// by headless tests — keep compiling unchanged. Exists so consumers (issue #31's AF
    /// execution and focus-peaking overlay) can read the source's own ground-truth sharpness
    /// instead of re-deriving it from <see cref="SequenceNumber"/>/<see cref="Pattern"/> with a
    /// duplicated formula.</summary>
    public double BlurRadius { get; init; }
}
