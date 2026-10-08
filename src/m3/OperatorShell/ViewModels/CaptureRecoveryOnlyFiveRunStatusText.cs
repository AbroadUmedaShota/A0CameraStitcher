using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.OperatorShell.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

/// <summary>
/// Operator-facing texts for every way the five-pair CaptureRecoveryOnly series can end
/// (GitHub Issue #251): the status text, the "アプリ内の保管" result row and the technical detail.
/// The coordinator's own detail is English and diagnostic, so it goes to the technical detail;
/// the status text says what happened, where the original images are, and what the operator can
/// do next. It only reads the result: the coordinator's decisions are not changed.
/// Wording rule: "保管しています" is for originals that are kept after the run stopped; "残っています"
/// is reserved for the texts after a failed save (OperatorShellViewModel).
/// </summary>
internal static class CaptureRecoveryOnlyFiveRunStatusText
{
    private const string ExitAfterInspectionText =
        "追加の撮影はできないため、カメラの状態を確認してからアプリを終了してください。";
    private const string ExitAfterConnectionInspectionText =
        "追加の撮影はできないため、カメラの接続と状態を確認してからアプリを終了してください。";
    private const string ExitText = "追加の撮影はできないため、アプリを終了してください。";
    private const string NoOriginalsText = "カメラから受け取れた原画像はありません。";
    private const string NoAggregateEvidenceText = "集約証跡は作成していません。";
    private const string RecheckSentence = "新しい撮影も自動再試行もせずに結果だけを確認します。";
    private const string NoContinuationAfterRecheckText = "確認の後も、この系列の続きは撮影できません。";

    private readonly record struct Summary(
        int Attempted,
        HardwareDualCaptureRecoveryOnlyExecution? Last,
        int CompletedPairs,
        int RetainedOriginals);

    internal static string Build(CaptureRecoveryOnlyFiveRunResult run)
    {
        ArgumentNullException.ThrowIfNull(run);
        var summary = Summarize(run);

        return run.Status switch
        {
            CaptureRecoveryOnlyFiveRunStatus.Completed => BuildCompleted(summary),
            CaptureRecoveryOnlyFiveRunStatus.Failed => BuildFailed(summary, run.EvidenceFiles is not null),
            CaptureRecoveryOnlyFiveRunStatus.HardwarePending => BuildHardwarePending(summary),
            CaptureRecoveryOnlyFiveRunStatus.Blocked => summary.Attempted == 0
                ? BuildNotStarted()
                : BuildStoppedBetweenPairs(summary),
            _ => "撮影・回収を停止しました。理由は技術情報に記録しました。" + ExitText,
        };
    }

    /// <summary>The "アプリ内の保管" row of the result list: the count of kept originals and whether an
    /// aggregate evidence was written. The internal verdict names stay in the technical detail.</summary>
    internal static string BuildExportResult(CaptureRecoveryOnlyFiveRunResult run)
    {
        ArgumentNullException.ThrowIfNull(run);
        var retained = Summarize(run).RetainedOriginals;
        var originals = retained == 0 ? "保管した原画像なし" : $"原画像{retained}枚を保管";
        var evidence = run.EvidenceFiles is null
            ? "集約証跡は未作成"
            : run.Status == CaptureRecoveryOnlyFiveRunStatus.Completed
                ? "集約証跡を保存"
                : "失敗までの集約証跡を保存";
        return originals + "・" + evidence;
    }

    /// <summary>Everything the operator-facing texts leave out: the coordinator's English detail, the
    /// internal verdict names and the evidence report path.</summary>
    internal static string BuildTechnicalDetail(string runId, CaptureRecoveryOnlyFiveRunResult run)
    {
        ArgumentNullException.ThrowIfNull(run);
        var last = run.LastOutcome;
        var aggregate = run.EvidenceFiles is null
            ? "none"
            : run.Status == CaptureRecoveryOnlyFiveRunStatus.Completed
                ? "SoftwareAggregatePartial"
                : "SoftwareAggregateFail";
        return
            $"runId={runId} / requested={CaptureRecoveryOnlyFiveRunCoordinator.RequestedCount} / " +
            $"attempted={run.AttemptedCount} / status={run.Status} / " +
            $"capturePurpose={HardwareDualCaptureRecoveryOnlyExecution.CapturePurpose} / " +
            $"stitchOutcome={HardwareDualCaptureRecoveryOnlyExecution.StitchOutcome} / " +
            $"a0QualityApproval={HardwareDualCaptureRecoveryOnlyExecution.A0QualityApproval} / " +
            "automatic retry count: 0 / " +
            $"lastTerminalState={(last is null ? "(none)" : last.TerminalState.ToString())} / " +
            $"lastFailure={(last is null ? "(none)" : last.FailureCode.ToString())} / " +
            $"aggregateEvidence={aggregate}" +
            (run.EvidenceFiles is null ? string.Empty : $" / evidenceReport={run.EvidenceFiles.ReportPath}") +
            $" / detail={run.Detail}";
    }

    private static Summary Summarize(CaptureRecoveryOnlyFiveRunResult run)
    {
        var last = run.LastOutcome;
        var attempted = run.AttemptedCount;
        var lastPairComplete = last is { Succeeded: true };
        var completedPairs = last is null ? 0 : Math.Max(0, lastPairComplete ? attempted : attempted - 1);
        var retained = completedPairs * 2 + (last is not null && !lastPairComplete ? last.Originals.Count : 0);
        return new Summary(attempted, last, completedPairs, retained);
    }

    private static string BuildCompleted(Summary s) =>
        $"{s.Attempted}組すべての撮影・回収・再検証が完了しました。" +
        $"撮影した原画像{s.RetainedOriginals}枚はアプリ内に保管し、集約証跡も保存しました。" +
        "合成は実施せず、A0品質は未承認です。" +
        "直前の1組は、「選択…」で保存先を選び、「原画像をこのPCのフォルダへ保存」で取り出せます。" +
        "追加の撮影はできないため、保存が済んだらアプリを終了してください。";

    private static string BuildFailed(Summary s, bool evidenceSaved)
    {
        var kept = (s.RetainedOriginals, evidenceSaved) switch
        {
            (0, true) => NoOriginalsText + "失敗までの集約証跡は保存しました。",
            (0, false) => NoOriginalsText + NoAggregateEvidenceText,
            (_, true) => $"受け取れた原画像{s.RetainedOriginals}枚はアプリ内に保管し、失敗までの集約証跡も保存しました。",
            _ => $"受け取れた原画像{s.RetainedOriginals}枚はアプリ内に保管しています。" + NoAggregateEvidenceText,
        };
        return $"{s.Attempted}組目の撮影・回収が途中で停止しました。" +
               CompletedPairsSentence(s.CompletedPairs) +
               RemainingSentence(s.Attempted) +
               kept +
               ExitAfterInspectionText;
    }

    private static string BuildHardwarePending(Summary s)
    {
        var last = s.Last;
        if (last is { RecoveryPending: true })
        {
            return OperatorShellViewModel.CaptureRecoveryOnlyUnconfirmedStatusText;
        }

        if (last is { BindingInvalidationReason: not DualBindingInvalidationReason.None })
        {
            // Defensive: a pair that finished and is still followed by the invalidation is rejected by
            // the workflow's terminal validation, so the production workflow does not reach the first
            // form. The sentence follows the same shape as the failed-pair form.
            var firstSentence = last.Succeeded
                ? $"機体照合が無効になったため、{s.Attempted}組目の撮影・回収の完了後に停止しました。" +
                  CompletedPairsSentence(s.Attempted)
                : $"機体照合が無効になったため、{s.Attempted}組目で撮影・回収を停止しました。" +
                  CompletedPairsSentence(s.CompletedPairs);
            return firstSentence +
                   RemainingSentence(s.Attempted) +
                   KeptWithoutEvidenceSentence(s) +
                   ExitAfterConnectionInspectionText;
        }

        // The pair was not started because the five-minute identity snapshot of this series ran out
        // before its shutter (before any transaction exists): not a hardware failure.
        if (last is { FailureCode: DualCameraFailureCode.IdentityNotReady })
        {
            return BuildIdentityExpired(s);
        }

        // The series stopped before its next pair because an earlier ID is still unconfirmed.
        // Without a last result the screen is the capture screen, whose main button re-checks that
        // ID; with one, the result panel is shown and the way back to the capture screen comes first.
        // The capture screen does not show the status message, so the caller also puts the first
        // form in the notice.
        if (last is null)
        {
            return "前回の撮影IDの結果が確定していないため、新しい撮影は始めていません。" +
                   "シャッターは1回も切っていません。" +
                   "「同じ撮影IDの結果を確認する」を押すと、" + RecheckSentence +
                   NoContinuationAfterRecheckText;
        }

        if (last.Succeeded)
        {
            return $"{s.Attempted}組の撮影・回収が完了した後、前回の撮影IDの結果が確定していないため、続きは始めていません。" +
                   $"撮影した原画像{s.RetainedOriginals}枚はアプリ内に保管しています。" +
                   "「撮り直しの準備へ」で撮影画面に戻り、「同じ撮影IDの結果を確認する」を押すと、" + RecheckSentence +
                   NoContinuationAfterRecheckText;
        }

        // The cause is not known here: the pair could not be completed for a reason that is not
        // named by a typed code (capability check, closing without a capture, and so on).
        return $"{s.Attempted}組目の撮影・回収を完了できなかったため、ここで停止しました。" +
               CompletedPairsSentence(s.CompletedPairs) +
               RemainingSentence(s.Attempted) +
               KeptWithoutEvidenceSentence(s) +
               ExitAfterConnectionInspectionText;
    }

    private static string BuildIdentityExpired(Summary s)
    {
        var shutter = s.Attempted == 1
            ? "シャッターは1回も切っていません。"
            : $"{s.CompletedPairs}組目までは完了しています。{s.Attempted}組目のシャッターは切っていません。";
        var kept = s.RetainedOriginals == 0
            ? string.Empty
            : $"撮影した原画像{s.RetainedOriginals}枚はアプリ内に保管しています。" + NoAggregateEvidenceText;
        return $"機体照合の有効時間が切れたため、{s.Attempted}組目の撮影は始めていません。" +
               shutter +
               "この組は受入の試行に数えません。" +
               kept +
               "追加の撮影はできないため、アプリを終了し、起動し直して機体照合からやり直してください。";
    }

    private static string BuildNotStarted() =>
        "新しい撮影を始められませんでした。シャッターは1回も切っていません。" +
        "理由は技術情報に記録しました。" +
        "この系列ではもう撮影できないため、アプリを終了してください。";

    private static string BuildStoppedBetweenPairs(Summary s) =>
        $"{s.Attempted}組の撮影・回収が完了した時点で、続きの撮影を始められなくなりました。" +
        RemainingSentence(s.Attempted) +
        $"撮影した原画像{s.RetainedOriginals}枚はアプリ内に保管しています。" +
        NoAggregateEvidenceText +
        "直前の1組は「原画像をこのPCのフォルダへ保存」で取り出せます。" +
        ExitText;

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

    private static string KeptWithoutEvidenceSentence(Summary s) =>
        (s.RetainedOriginals == 0
            ? NoOriginalsText
            : $"受け取れた原画像{s.RetainedOriginals}枚はアプリ内に保管しています。") +
        NoAggregateEvidenceText;
}
