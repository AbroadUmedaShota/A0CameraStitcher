using System.Windows.Media;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

public sealed class ProgressStepViewModel(string id, string label) : ObservableObject
{
    private string _statusText = "待機";
    private Brush _background = Brushes.White;
    private Brush _foreground = Brushes.Black;
    private Brush _borderBrush = Brushes.LightGray;

    public string Id { get; } = id;

    public string Label { get; } = label;

    public string StatusText { get => _statusText; private set => SetProperty(ref _statusText, value); }

    public Brush Background { get => _background; private set => SetProperty(ref _background, value); }

    public Brush Foreground { get => _foreground; private set => SetProperty(ref _foreground, value); }

    public Brush BorderBrush { get => _borderBrush; private set => SetProperty(ref _borderBrush, value); }

    public void SetPending() => SetVisual("待機", Brushes.White, Brushes.Black, Brushes.LightGray);

    public void SetCompleted() => SetVisual("完了", Brushes.Honeydew, Brushes.DarkGreen, Brushes.SeaGreen);

    public void SetCurrent() => SetVisual("処理中", Brushes.LightGoldenrodYellow, Brushes.Black, Brushes.DarkOrange);

    public void SetFailure() => SetVisual("失敗", Brushes.MistyRose, Brushes.DarkRed, Brushes.Firebrick);

    private void SetVisual(string status, Brush background, Brush foreground, Brush border)
    {
        StatusText = status;
        Background = background;
        Foreground = foreground;
        BorderBrush = border;
    }
}
