namespace A0CameraStitcher.M3.Foundation;

public enum OperatorUiState
{
    AwaitingSafetyAck,
    CheckingReadiness,
    NotReady,
    Ready,
    ReadyWithCorrection,
    Capturing,
    Stitching,
    Review,
    FailedPartial,
    Degraded,
}

public enum SetupAssessmentStatus
{
    Ready,
    ReadyWithCorrection,
    PhysicalAdjustmentRequired,
}

public enum OperatorWarningSeverity
{
    Info,
    Caution,
    Blocker,
}

public sealed record OperatorNotice(
    OperatorWarningSeverity Severity,
    string Code,
    string Message);

public sealed record CameraReadiness(
    string Alias,
    bool Connected,
    bool IdentityBound,
    bool SettingsMatch,
    bool CardKnownEmpty,
    bool LiveViewActive = false);

public sealed record RigProfileReadiness(
    string ProfileId,
    string Version,
    DateOnly ExpiresOn,
    bool Approved,
    bool Consistent);

public sealed record SetupAssessment(
    SetupAssessmentStatus Status,
    string Summary,
    IReadOnlyList<string> PlannedCorrections,
    IReadOnlyList<string> PhysicalAdjustments);

public sealed record ReadinessSnapshot
{
    public required bool SafetyAcknowledged { get; init; }

    public required IReadOnlyList<CameraReadiness> Cameras { get; init; }

    public required RigProfileReadiness Profile { get; init; }

    public required SetupAssessment Setup { get; init; }

    public required string OutputDirectory { get; init; }

    public required bool OutputDirectoryValid { get; init; }

    public required bool HasActiveTransaction { get; init; }

    public required bool CameraStateRequiresInspection { get; init; }

    public required IReadOnlyList<OperatorNotice> Notices { get; init; }
}

public sealed record OperatorActionDecision(bool Allowed, string DisabledReason)
{
    public static OperatorActionDecision Permit() => new(true, string.Empty);

    public static OperatorActionDecision Block(string reason) => new(false, reason);
}

public sealed record OperatorActionAvailability
{
    public required OperatorActionDecision Capture { get; init; }

    public required OperatorActionDecision LiveView { get; init; }

    public required OperatorActionDecision Export { get; init; }

    public required OperatorActionDecision Restitch { get; init; }

    public required OperatorActionDecision PrepareNewCapture { get; init; }

    public required OperatorActionDecision OpenMaintenance { get; init; }
}

public static class OperatorReadinessEvaluator
{
    public static IReadOnlyList<OperatorNotice> BuildNotices(ReadinessSnapshot snapshot, DateOnly today)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        var notices = new List<OperatorNotice>();

        if (!snapshot.SafetyAcknowledged)
        {
            notices.Add(new(OperatorWarningSeverity.Blocker, "SafetyAckRequired", "起動時の排他使用への同意が必要です。"));
        }

        foreach (var camera in snapshot.Cameras)
        {
            if (!camera.Connected)
            {
                notices.Add(new(OperatorWarningSeverity.Blocker, "CameraMissing", $"{camera.Alias} が接続されていません。"));
            }
            else if (!camera.IdentityBound)
            {
                notices.Add(new(OperatorWarningSeverity.Blocker, "IdentityUnbound", $"{camera.Alias} のidentity bindingが未完了です。"));
            }

            if (!camera.SettingsMatch)
            {
                notices.Add(new(OperatorWarningSeverity.Blocker, "SettingsMismatch", $"{camera.Alias} の撮影設定がリグプロファイルと一致しません。"));
            }

            if (!camera.CardKnownEmpty)
            {
                notices.Add(new(OperatorWarningSeverity.Blocker, "CardNotKnownEmpty", $"{camera.Alias} のカードがempty-spoolと確認できません。"));
            }
        }

        if (!snapshot.Profile.Approved || !snapshot.Profile.Consistent || snapshot.Profile.ExpiresOn < today)
        {
            notices.Add(new(OperatorWarningSeverity.Blocker, "ProfileInvalid", "リグプロファイルが未承認、不整合、または期限切れです。"));
        }

        if (snapshot.Setup.Status == SetupAssessmentStatus.PhysicalAdjustmentRequired)
        {
            notices.Add(new(OperatorWarningSeverity.Blocker, "PhysicalAdjustmentRequired", "自動補正範囲を超えています。表示された方向へ物理調整してください。"));
        }
        else if (snapshot.Setup.Status == SetupAssessmentStatus.ReadyWithCorrection)
        {
            notices.Add(new(OperatorWarningSeverity.Caution, "AutomaticCorrectionPlanned", "自動補正範囲内です。表示された補正を適用して撮影できます。"));
        }

        if (!snapshot.OutputDirectoryValid)
        {
            notices.Add(new(OperatorWarningSeverity.Blocker, "OutputInvalid", "保存先を読み書きできません。"));
        }

        if (snapshot.HasActiveTransaction)
        {
            notices.Add(new(OperatorWarningSeverity.Blocker, "ActiveTransaction", "別のtransactionが処理中です。"));
        }

        if (snapshot.CameraStateRequiresInspection)
        {
            notices.Add(new(OperatorWarningSeverity.Blocker, "CameraInspectionRequired", "カードまたはSDK状態の安全確認が必要です。"));
        }

        notices.Add(new(OperatorWarningSeverity.Info, "PreviewIsNotOriginal", "Live Viewはプレビューであり、原画像や合成入力には使用しません。"));
        notices.Add(new(OperatorWarningSeverity.Info, "NoShutterSync", "二台の実シャッター時刻差は保証しません。"));
        return notices.Concat(snapshot.Notices).ToArray();
    }

    public static OperatorUiState GetReadyState(ReadinessSnapshot snapshot, DateOnly today)
    {
        var blockers = BuildNotices(snapshot, today).Any(notice => notice.Severity == OperatorWarningSeverity.Blocker);
        if (blockers)
        {
            return snapshot.SafetyAcknowledged ? OperatorUiState.NotReady : OperatorUiState.AwaitingSafetyAck;
        }

        return snapshot.Setup.Status == SetupAssessmentStatus.ReadyWithCorrection
            ? OperatorUiState.ReadyWithCorrection
            : OperatorUiState.Ready;
    }

    public static OperatorActionAvailability Evaluate(
        ReadinessSnapshot snapshot,
        OperatorUiState state,
        DateOnly today,
        bool hasStitchResult,
        bool canRestitch)
    {
        var blocker = BuildNotices(snapshot, today)
            .FirstOrDefault(notice => notice.Severity == OperatorWarningSeverity.Blocker);
        var active = state is OperatorUiState.Capturing or OperatorUiState.Stitching;
        var capture = !active && blocker is null && state is OperatorUiState.Ready or OperatorUiState.ReadyWithCorrection
            ? OperatorActionDecision.Permit()
            : OperatorActionDecision.Block(active ? "処理中は新しい撮影を開始できません。" : blocker?.Message ?? "新しい撮影を準備してください。");

        return new OperatorActionAvailability
        {
            Capture = capture,
            LiveView = !active && snapshot.SafetyAcknowledged && !snapshot.CameraStateRequiresInspection
                ? OperatorActionDecision.Permit()
                : OperatorActionDecision.Block(active ? "処理中はLive Viewを変更できません。" : "排他同意またはSDK状態確認が必要です。"),
            Export = !active && state == OperatorUiState.Review && hasStitchResult
                ? OperatorActionDecision.Permit()
                : OperatorActionDecision.Block("検証済みの合成結果を確認してから保存できます。"),
            Restitch = !active && canRestitch
                ? OperatorActionDecision.Permit()
                : OperatorActionDecision.Block("左右両方の保持原画像が必要です。"),
            PrepareNewCapture = !active && state is OperatorUiState.Review or OperatorUiState.FailedPartial or OperatorUiState.Degraded
                ? OperatorActionDecision.Permit()
                : OperatorActionDecision.Block(active ? "処理完了まで待ってください。" : "結果確定後に使用できます。"),
            OpenMaintenance = !active
                ? OperatorActionDecision.Permit()
                : OperatorActionDecision.Block("active transaction中は保守画面を開けません。"),
        };
    }
}

public sealed record CaptureOutcome(
    Guid TransactionId,
    SimulatedTransactionState State,
    IReadOnlyList<string> RetainedOriginalAliases,
    string OperatorMessage,
    string? ErrorCode,
    DateTimeOffset CompletedAt);

public sealed record StitchOutcome(
    Guid StitchJobId,
    bool Succeeded,
    string OperatorMessage,
    IReadOnlyList<string> AppliedCorrections,
    string? ErrorCode);

public sealed record ExportOutcome(
    Guid ExportJobId,
    bool Succeeded,
    string? OutputPath,
    string OperatorMessage,
    string? ErrorCode);
