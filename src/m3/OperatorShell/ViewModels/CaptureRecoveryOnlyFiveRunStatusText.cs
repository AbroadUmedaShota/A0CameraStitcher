using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.OperatorShell.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

/// <summary>
/// Operator-facing status text for every way the five-pair CaptureRecoveryOnly series can end
/// (GitHub Issue #251). The coordinator's own detail is English and diagnostic, so it goes to the
/// technical detail; this text says what happened, where the original images are, and what the
/// operator can do next. It only reads the result: the coordinator's decisions are not changed.
/// </summary>
internal static class CaptureRecoveryOnlyFiveRunStatusText
{
    internal const string NoFurtherCaptureText = "追加の撮影はできません。";

    private const string ExitAfterInspectionText = "カメラの状態を確認し、アプリを終了してください。";
    private const string ExitAfterConnectionInspectionText = "カメラの接続と状態を確認し、アプリを終了してください。";
    private const string NoOriginalsText = "カメラから受け取れた原画像はありません。";

    internal static string Build(CaptureRecoveryOnlyFiveRunResult run)
    {
        ArgumentNullException.ThrowIfNull(run);
        var last = run.LastOutcome;
        var attempted = run.AttemptedCount;
        var lastPairComplete = last is { Succeeded: true };
        var completedPairs = last is null ? 0 : Math.Max(0, lastPairComplete ? attempted : attempted - 1);
        var retainedOriginals = completedPairs * 2 + (last is not null && !lastPairComplete ? last.Originals.Count : 0);

        return run.Status switch
        {
            CaptureRecoveryOnlyFiveRunStatus.Completed => BuildCompleted(attempted, retainedOriginals),
            CaptureRecoveryOnlyFiveRunStatus.Failed => BuildFailed(
                attempted, completedPairs, retainedOriginals, run.EvidenceFiles is not null),
            CaptureRecoveryOnlyFiveRunStatus.HardwarePending => BuildHardwarePending(
                last, attempted, completedPairs, retainedOriginals),
            CaptureRecoveryOnlyFiveRunStatus.Blocked => attempted == 0
                ? BuildNotStarted()
                : BuildStoppedBetweenPairs(attempted, retainedOriginals),
            _ => "撮影・回収を停止しました。詳細は技術情報をご覧ください。",
        };
    }

    private static string BuildCompleted(int attempted, int retainedOriginals) =>
        $"{attempted}組すべての撮影・回収・再検証が完了しました。" +
        $"撮影した原画像{retainedOriginals}枚はアプリ内に保管しています。" +
        "合成は実施せず、A0品質は未承認です。" +
        NoFurtherCaptureText +
        "保存先を選ぶと、「原画像をこのPCのフォルダへ保存」で直前の1組を取り出せます。";

    private static string BuildFailed(int attempted, int completedPairs, int retainedOriginals, bool evidenceSaved) =>
        $"{attempted}組目の撮影・回収が途中で停止しました。" +
        CompletedPairsSentence(completedPairs) +
        RemainingSentence(attempted) +
        RetainedAfterStopSentence(retainedOriginals) +
        NoFurtherCaptureText +
        (evidenceSaved ? "失敗までの集約証跡は保存済みです。" : string.Empty) +
        ExitAfterInspectionText;

    private static string BuildHardwarePending(
        HardwareDualCaptureRecoveryOnlyExecution? last,
        int attempted,
        int completedPairs,
        int retainedOriginals)
    {
        if (last is { RecoveryPending: true })
        {
            return OperatorShellViewModel.CaptureRecoveryOnlyUnconfirmedStatusText;
        }

        if (last is { BindingInvalidationReason: not DualBindingInvalidationReason.None })
        {
            // A pair can finish and still be followed by the invalidation; then the pair itself is not lost.
            var firstSentence = last.Succeeded
                ? $"{attempted}組目の撮影・回収は完了しましたが、機体照合が無効になったため、ここで停止しました。"
                : $"機体照合が無効になったため、{attempted}組目で撮影・回収を停止しました。" +
                  CompletedPairsSentence(completedPairs);
            return firstSentence +
                   RemainingSentence(attempted) +
                   RetainedAfterStopSentence(retainedOriginals) +
                   NoFurtherCaptureText +
                   ExitAfterConnectionInspectionText;
        }

        if (last is { TerminalState: DualHardwareCaptureTerminalState.HardwarePending })
        {
            return $"{attempted}組目の撮影・回収を完了できませんでした。カメラの状態を確認できないため、ここで停止しました。" +
                   CompletedPairsSentence(completedPairs) +
                   RemainingSentence(attempted) +
                   RetainedAfterStopSentence(retainedOriginals) +
                   NoFurtherCaptureText +
                   ExitAfterConnectionInspectionText;
        }

        // The series stopped before its next pair because an earlier ID is still unconfirmed.
        // Without a last result the screen is the capture screen, whose main button re-checks that
        // ID; with one, the result panel is shown and the way back to the capture screen comes first.
        const string recheckSentence =
            "新しい撮影も自動再試行もせずに結果だけを確認します。";
        if (last is null)
        {
            return "前回の撮影IDの結果が確定していないため、新しい撮影は始めていません。シャッターは切っていません。" +
                   "「同じ撮影IDの結果を確認する」を押すと、" + recheckSentence;
        }

        return $"{completedPairs}組の撮影・回収の後、前回の撮影IDの結果が確定していないため、続きは始めていません。" +
               $"撮影した原画像{retainedOriginals}枚はアプリ内に保管しています。" +
               "「撮り直しの準備へ」で撮影画面に戻り、「同じ撮影IDの結果を確認する」を押すと、" + recheckSentence;
    }

    private static string BuildNotStarted() =>
        "新しい撮影を始められませんでした。シャッターは1回も切っていません。" +
        "主ボタンの下に出ている理由を確認してください。";

    private static string BuildStoppedBetweenPairs(int attempted, int retainedOriginals) =>
        $"{attempted}組の撮影・回収が完了した時点で、続きの撮影を始められなくなりました。" +
        RemainingSentence(attempted) +
        $"撮影した原画像{retainedOriginals}枚はアプリ内に保管しています。" +
        NoFurtherCaptureText +
        "アプリを終了してください。";

    private static string CompletedPairsSentence(int completedPairs) =>
        completedPairs > 0 ? $"{completedPairs}組目までは完了しています。" : string.Empty;

    // The series never retries and never continues after its first stop, so what is left unshot is
    // the rest of the requested pairs.
    private static string RemainingSentence(int attempted)
    {
        var remaining = CaptureRecoveryOnlyFiveRunCoordinator.RequestedCount - attempted;
        return remaining > 0
            ? $"残りの{remaining}組は撮影せず、自動再試行もしていません。"
            : "自動再試行はしていません。";
    }

    private static string RetainedAfterStopSentence(int retainedOriginals) =>
        retainedOriginals == 0
            ? NoOriginalsText
            : $"受け取れた原画像は{retainedOriginals}枚です。撮影した原画像はアプリ内に残っています。";
}
