using System.IO;
using System.Windows;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

namespace A0CameraStitcher.M3.OperatorShell;

public partial class MainWindow : Window
{
    private readonly CancellationTokenSource _lifetime = new();
    private readonly OperatorShellViewModel _viewModel;
    private readonly HardwareSingleAppSessionLease? _sessionLease;
    private readonly DualCameraAgentLifecycle? _dualAgentLifecycle;

    public MainWindow(DualCameraExecutionEnvironment environment = DualCameraExecutionEnvironment.TestSynthetic)
    {
        // HardwareDual shares the same exclusive OS-lease Single uses: at most one
        // hardware operator window (Single or Dual) may be open in this Windows logon
        // session, so a mode switch can only start Dual after Single has fully exited.
        _sessionLease = environment == DualCameraExecutionEnvironment.HardwareDual
            ? HardwareSingleAppSessionLease.Acquire()
            : null;
        try
        {
            InitializeComponent();
            var simulatedRoot = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "A0CameraStitcher",
                "m3-simulated");
            var dualProductRoot = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "A0CameraStitcher",
                environment == DualCameraExecutionEnvironment.HardwareDual
                    ? "dual-camera-hardware-products"
                    : "dual-camera-test-synthetic-products");
            if (environment == DualCameraExecutionEnvironment.HardwareDual)
            {
                _dualAgentLifecycle = new DualCameraAgentLifecycle(
                    ResolveDualCameraAgentExecutablePath(),
                    Path.Combine(dualProductRoot, "agent-pair-journal"),
                    Path.Combine(dualProductRoot, "camera-agent", "approved-dual-capture-profile.json"),
                    Path.Combine(dualProductRoot, "phase0", "dual-identity-proof.json"));
            }
            // The identity source is left unconfigured (HardwarePending) in production:
            // no real DualCamera identity provider exists yet, so HardwareDual never
            // reaches Ready and this lifecycle is constructed but never launches a
            // process or reaches a camera. Connecting a real identity provider and
            // flipping this to Ready is explicitly out of scope for this change.
            _viewModel = new OperatorShellViewModel(
                new SimulationFoundationService(simulatedRoot),
                DualCameraProductComposition.Create(dualProductRoot, environment, _dualAgentLifecycle));
            DataContext = _viewModel;
            Loaded += OnLoaded;
            Closed += OnClosed;
        }
        catch
        {
            _sessionLease?.Dispose();
            throw;
        }
    }

    private static string ResolveDualCameraAgentExecutablePath()
    {
        var configuredPath = Environment.GetEnvironmentVariable("A0_DUAL_CAMERA_AGENT_PATH");
        return string.IsNullOrWhiteSpace(configuredPath)
            ? Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.DualCameraAgent.exe")
            : Path.GetFullPath(configuredPath);
    }

    private async void OnLoaded(object sender, RoutedEventArgs eventArgs)
    {
        Loaded -= OnLoaded;
        await _viewModel.InitializeAsync(_lifetime.Token);
    }

    private async void OnClosed(object? sender, EventArgs eventArgs)
    {
        _lifetime.Cancel();
        try
        {
            if (_dualAgentLifecycle is not null)
            {
                try
                {
                    await _dualAgentLifecycle.DisposeAsync();
                }
                catch (Exception exception) when (exception is not OutOfMemoryException)
                {
                    // Never force-kill an Agent whose pair-dispatch state may be
                    // ambiguous. Durable recovery resolves it on the next launch.
                }
            }
        }
        finally
        {
            _lifetime.Dispose();
            _sessionLease?.Dispose();
        }
    }
}
