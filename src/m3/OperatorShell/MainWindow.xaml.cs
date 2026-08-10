using System.IO;
using System.Windows;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

namespace A0CameraStitcher.M3.OperatorShell;

public partial class MainWindow : Window
{
    private readonly CancellationTokenSource _lifetime = new();
    private readonly OperatorShellViewModel _viewModel;

    public MainWindow()
    {
        InitializeComponent();
        var simulatedRoot = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "A0CameraStitcher",
            "m3-simulated");
        var dualProductRoot = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "A0CameraStitcher",
            "dual-camera-test-synthetic-products");
        _viewModel = new OperatorShellViewModel(
            new SimulationFoundationService(simulatedRoot),
            DualCameraProductComposition.Create(dualProductRoot));
        DataContext = _viewModel;
        Loaded += OnLoaded;
        Closed += OnClosed;
    }

    private async void OnLoaded(object sender, RoutedEventArgs eventArgs)
    {
        Loaded -= OnLoaded;
        await _viewModel.InitializeAsync(_lifetime.Token);
    }

    private void OnClosed(object? sender, EventArgs eventArgs)
    {
        _lifetime.Cancel();
        _lifetime.Dispose();
    }
}
