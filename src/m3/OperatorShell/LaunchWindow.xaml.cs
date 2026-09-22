using System.Windows;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.Foundation.DualCamera;

namespace A0CameraStitcher.M3.OperatorShell;

public partial class LaunchWindow : Window
{
    private readonly string _cameraAgentExecutablePath;
    private HardwareSingleCandidateInspection _candidateInspection;

    public LaunchWindow(string cameraAgentExecutablePath)
    {
        InitializeComponent();
        _cameraAgentExecutablePath = cameraAgentExecutablePath;
        _candidateInspection = HardwareSingleCandidateManifest.Inspect(cameraAgentExecutablePath);
        ApplyCandidateInspection(_candidateInspection);
    }

    private void ApplyCandidateInspection(HardwareSingleCandidateInspection inspection)
    {
        AgentPathText.Text = "実行候補を読み取り確認（SDK・カメラは起動・照会しません）";
        AgentAvailabilityText.Text = inspection.AvailabilityText;
        AgentAvailabilityText.Foreground = inspection.CanStartHardware
            ? System.Windows.Media.Brushes.DarkOrange
            : System.Windows.Media.Brushes.DarkRed;
        HardwareSingleButton.IsEnabled = inspection.CanStartHardware;
    }

    private void OnHardwareSingleClick(object sender, RoutedEventArgs eventArgs)
    {
        _candidateInspection = HardwareSingleCandidateManifest.Inspect(_cameraAgentExecutablePath);
        ApplyCandidateInspection(_candidateInspection);
        if (!_candidateInspection.CanStartHardware)
        {
            MessageBox.Show(
                _candidateInspection.AvailabilityText,
                "A0 Camera Stitcher — 実機一台構成は開始できません",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
            return;
        }

        OpenAndClose(() => new HardwareSingleCameraWindow(_cameraAgentExecutablePath));
    }

    private void OnSimulatedClick(object sender, RoutedEventArgs eventArgs) =>
        OpenAndClose(() => new MainWindow());

    private void OnHardwareDualClick(object sender, RoutedEventArgs eventArgs)
    {
        // The HardwareDual binding host needs an operator-provided, existing WPD
        // identity map. This launcher deliberately has no path picker or default:
        // guessing one would weaken the fixed-local, explicit-map boundary.
        MessageBox.Show(
            "実機2台は --hardware-dual --wpd-camera-map <既存の固定ローカルmap> を指定して起動してください。",
            "A0 Camera Stitcher — 実機2台の起動条件",
            MessageBoxButton.OK,
            MessageBoxImage.Information);
    }

    // Every launch path is funneled through here so the exclusive hardware-operator
    // session lease (acquired by HardwareSingleCameraWindow and, for HardwareDual, by
    // MainWindow) has exactly one catch site. Without it, a busy lease throws
    // HardwareSingleAppSessionBusyException out of the WPF click handler and crashes
    // the whole process instead of showing the existing "already in use" dialog.
    private void OpenAndClose(Func<Window> createWindow)
    {
        Window nextWindow;
        try
        {
            nextWindow = createWindow();
        }
        catch (HardwareSingleAppSessionBusyException exception)
        {
            MessageBox.Show(
                exception.Message,
                "A0 Camera Stitcher — 実機操作は既に使用中",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
            return;
        }
        catch (ArgumentException exception) when (exception.Message == CameraAgentExecutablePolicy.InvalidExecutableMessage)
        {
            // MainWindow(HardwareDual) の ctor は CameraAgentExecutablePolicy.Resolve で
            // Dual Agent EXE を検証し、不在・不正パスなら ArgumentException を投げる
            // （issue #142 症状2）。このメソッドの意図（上のコメント参照）どおり、
            // プロセスをクラッシュさせず既存のダイアログ経路へ落とす。
            //
            // 素の ArgumentException 型ではなく Resolve 自身の再スロー判定
            // （CameraAgentExecutablePolicy.Resolve 内の同種フィルタ）に揃えたメッセージ番兵で
            // 絞り込む（PR #152 レビュー指摘・軽微3）。型だけで絞ると ArgumentNullException /
            // ArgumentOutOfRangeException（ArgumentException のサブクラス）や、
            // createWindow() 内の無関係なプログラミングバグまで「起動引数エラー」に
            // 化けて飲み込んでしまう。
            MessageBox.Show(
                exception.Message,
                "A0 Camera Stitcher — 起動引数エラー",
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            return;
        }
        Application.Current.MainWindow = nextWindow;
        nextWindow.Show();
        Close();
    }
}
