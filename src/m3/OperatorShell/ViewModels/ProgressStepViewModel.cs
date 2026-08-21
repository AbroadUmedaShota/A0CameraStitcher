namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

public sealed class ProgressStepViewModel(string id, string label) : ObservableObject
{
    private string _statusText = "待機";
    private string _stateKey = PendingState;

    public const string PendingState = "pending";
    public const string CompletedState = "completed";
    public const string CurrentState = "current";
    public const string FailureState = "failure";
    public const string SkippedState = "skipped";

    public string Id { get; } = id;

    public string Label { get; } = label;

    public string StatusText { get => _statusText; private set => SetProperty(ref _statusText, value); }

    /// <summary>段の状態。色はここでは決めず、画面側がテーマのトークンへ解決する。
    /// ViewModel が Brush を持つと、画面の配色を変えるたびに ViewModel を触ることになり、
    /// 明色前提の値が暗い背景の上へそのまま出る事故が起きる。</summary>
    public string StateKey { get => _stateKey; private set => SetProperty(ref _stateKey, value); }

    public void SetPending() => SetVisual("待機", PendingState);

    public void SetCompleted() => SetVisual("完了", CompletedState);

    public void SetCurrent() => SetVisual("処理中", CurrentState);

    public void SetFailure() => SetVisual("失敗", FailureState);

    public void SetSkipped() => SetVisual("対象外", SkippedState);

    private void SetVisual(string status, string stateKey)
    {
        StatusText = status;
        StateKey = stateKey;
    }
}
