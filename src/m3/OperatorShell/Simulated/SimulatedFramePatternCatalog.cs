namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// Maps each <see cref="SimulatedFramePattern"/> to the Japanese label shown in the
/// development-only pattern switcher (Setup/校正 tab). Kept separate from the ViewModel so
/// both the WPF binding surface and the frame source can share one source of truth.
/// </summary>
public static class SimulatedFramePatternCatalog
{
    public static readonly IReadOnlyList<(string Label, SimulatedFramePattern Pattern)> Entries =
    [
        ("正対原稿サンプル", SimulatedFramePattern.FrontalDocument),
        ("傾き原稿サンプル ROLL -6°", SimulatedFramePattern.TiltedDocumentRollMinus6),
        ("傾き原稿サンプル ROLL -3°", SimulatedFramePattern.TiltedDocumentRollMinus3),
        ("傾き原稿サンプル ROLL +3°", SimulatedFramePattern.TiltedDocumentRollPlus3),
        ("傾き原稿サンプル ROLL +6°", SimulatedFramePattern.TiltedDocumentRollPlus6),
        ("ボケ→合焦遷移", SimulatedFramePattern.BlurToFocusTransition),
    ];

    public static IReadOnlyList<string> Labels { get; } = Entries.Select(entry => entry.Label).ToArray();

    public static SimulatedFramePattern DefaultPattern => Entries[0].Pattern;

    public static string DefaultLabel => Entries[0].Label;

    public static bool TryGetPattern(string label, out SimulatedFramePattern pattern)
    {
        foreach (var entry in Entries)
        {
            if (string.Equals(entry.Label, label, StringComparison.Ordinal))
            {
                pattern = entry.Pattern;
                return true;
            }
        }

        pattern = default;
        return false;
    }
}
