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

    /// <summary>A repeating blur-to-focus transition, for focus peaking verification.</summary>
    BlurToFocusTransition,
}
