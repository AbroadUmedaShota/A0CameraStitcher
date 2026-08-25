namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// Selects which synthetic scene the SIMULATED live view frame source renders. Every
/// pattern is generated entirely in code at runtime; none reference bitmap assets.
/// </summary>
public enum SimulatedFramePattern
{
    /// <summary>A document sample photographed straight-on (no rotation).</summary>
    FrontalDocument,

    /// <summary>A document sample rotated -6 degrees, for edge/tilt detection verification.</summary>
    TiltedDocumentRollMinus6,

    /// <summary>A document sample rotated -3 degrees, for edge/tilt detection verification.</summary>
    TiltedDocumentRollMinus3,

    /// <summary>A document sample rotated +3 degrees, for edge/tilt detection verification.</summary>
    TiltedDocumentRollPlus3,

    /// <summary>A document sample rotated +6 degrees, for edge/tilt detection verification.</summary>
    TiltedDocumentRollPlus6,

    /// <summary>A document sample meant to be rotated by an arbitrary angle supplied at render
    /// time, instead of one of the four fixed magnitudes above. Test-only: never appears in
    /// <see cref="SimulatedFramePatternCatalog"/> (so it is not selectable from the Setup/校正
    /// tab's pattern switcher). Rendering an actual non-zero angle for this pattern requires
    /// <see cref="SimulatedTestImageFrameSource.CreateTiltedDocumentFrameForTesting"/> — which
    /// GitHub Issue #84's <c>DocumentTiltDetector</c> accuracy tests use to probe angles between
    /// the fixed magnitudes' quantization grid — since the angle isn't carried by the enum
    /// value itself; going through the ordinary
    /// <see cref="SimulatedTestImageFrameSource.CreateFrame"/> with this pattern instead falls
    /// through to an unrotated (0°) frame, the same as any other pattern with no fixed
    /// angle.</summary>
    TiltedDocumentCustomRoll,

    /// <summary>A repeating blur-to-focus transition, for focus peaking verification.</summary>
    BlurToFocusTransition,
}
