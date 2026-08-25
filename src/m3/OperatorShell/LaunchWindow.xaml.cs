using System.IO;
using System.Windows;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.Foundation.DualCamera;

namespace A0CameraStitcher.M3.OperatorShell;

public partial class LaunchWindow : Window
{
    private readonly string _cameraAgentExecutablePath;

    public LaunchWindow(string cameraAgentExecutablePath)
    {
        InitializeComponent();
        _cameraAgentExecutablePath = cameraAgentExecutablePath;
        AgentPathText.Text = cameraAgentExecutablePath;
        AgentAvailabilityText.Text = File.Exists(cameraAgentExecutablePath)
            ? "検出済み（選択するまでカメラ操作は開始しません）"
            : "未検出（実機画面はfail-closedで撮影を無効化します）";
        AgentAvailabilityText.Foreground = File.Exists(cameraAgentExecutablePath)
            ? System.Windows.Media.Brushes.DarkGreen
            : System.Windows.Media.Brushes.DarkRed;
    }

    private void OnHardwareSingleClick(object sender, RoutedEventArgs eventArgs) =>
        OpenAndClose(() => new HardwareSingleCameraWindow(_cameraAgentExecutablePath));

    private void OnSimulatedClick(object sender, RoutedEventArgs eventArgs) =>
        OpenAndClose(() => new MainWindow());

    private void OnHardwareDualClick(object sender, RoutedEventArgs eventArgs) =>
        OpenAndClose(() => new MainWindow(DualCameraExecutionEnvironment.HardwareDual));

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
