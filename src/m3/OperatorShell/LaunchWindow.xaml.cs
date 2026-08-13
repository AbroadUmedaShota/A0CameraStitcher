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

    private void OnHardwareSingleClick(object sender, RoutedEventArgs eventArgs)
    {
        try
        {
            OpenAndClose(new HardwareSingleCameraWindow(_cameraAgentExecutablePath));
        }
        catch (HardwareSingleAppSessionBusyException exception)
        {
            MessageBox.Show(
                exception.Message,
                "A0 Camera Stitcher — 実機操作は既に使用中",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
        }
    }

    private void OnSimulatedClick(object sender, RoutedEventArgs eventArgs) =>
        OpenAndClose(new MainWindow());

    private void OnHardwareDualClick(object sender, RoutedEventArgs eventArgs) =>
        OpenAndClose(new MainWindow(DualCameraExecutionEnvironment.HardwareDual));

    private void OpenAndClose(Window nextWindow)
    {
        Application.Current.MainWindow = nextWindow;
        nextWindow.Show();
        Close();
    }
}
