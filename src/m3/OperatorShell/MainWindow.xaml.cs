using System.IO;
using System.Windows;
using System.Windows.Input;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.OperatorShell.Simulated;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

namespace A0CameraStitcher.M3.OperatorShell;

public partial class MainWindow : Window
{
    private readonly CancellationTokenSource _lifetime = new();
    private readonly OperatorShellViewModel _viewModel;
    private readonly HardwareSingleAppSessionLease? _sessionLease;
    private readonly DualCameraAgentLifecycle? _dualAgentLifecycle;
    private readonly ISimulatedLiveViewFrameSource _liveViewFrameSource = new SimulatedTestImageFrameSource();
    private readonly ISimulatedLiveViewFramePump _liveViewFramePump = new SimulatedLiveViewFramePump();

    // Target reticle (□) drag state (issue #30). Mouse capture keeps MouseMove/MouseUp routed
    // to whichever element started the drag even if the pointer leaves its bounds, so a single
    // pair of shared handlers below serves both the stage's coarse drag and the loupe's fine
    // drag — only the reference area (for delta normalization) and the apply delegate (coarse
    // vs. fine) differ per drag source. All target-position math itself lives in the
    // ViewModel (MoveTargetByStageDrag/MoveTargetByLoupeDrag); this code-behind only turns
    // mouse pixel deltas into normalized 0..1 deltas.
    private FrameworkElement? _targetDragElement;
    private FrameworkElement? _targetDragReferenceArea;
    private Point _targetDragLastPoint;
    private Action<double, double>? _targetDragApply;

    public MainWindow(
        DualCameraExecutionEnvironment environment = DualCameraExecutionEnvironment.TestSynthetic,
        string? dualCameraAgentExecutablePath = null)
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
                    CameraAgentExecutablePolicy.Resolve(
                        AppContext.BaseDirectory,
                        dualCameraAgentExecutablePath ?? Path.Combine(
                            AppContext.BaseDirectory,
                            "A0CameraStitcher.DualCameraAgent.exe")),
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
                DualCameraProductComposition.Create(dualProductRoot, environment, _dualAgentLifecycle),
                liveViewFramePump: _liveViewFramePump,
                liveViewFrameSource: _liveViewFrameSource);
            DataContext = _viewModel;
            Loaded += OnLoaded;
            Closed += OnClosed;
        }
        catch
        {
            _liveViewFramePump.Dispose();
            _sessionLease?.Dispose();
            throw;
        }
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
            _liveViewFramePump.Dispose();
            _lifetime.Dispose();
            _sessionLease?.Dispose();
        }
    }

    private void StageDragHandle_MouseLeftButtonDown(object sender, MouseButtonEventArgs eventArgs) =>
        BeginTargetDrag(sender, eventArgs, StageDisplayArea, _viewModel.MoveTargetByStageDrag);

    private void LoupeDragHandle_MouseLeftButtonDown(object sender, MouseButtonEventArgs eventArgs) =>
        BeginTargetDrag(sender, eventArgs, LoupeDisplayArea, _viewModel.MoveTargetByLoupeDrag);

    private void BeginTargetDrag(object sender, MouseButtonEventArgs eventArgs, FrameworkElement referenceArea, Action<double, double> applyDelta)
    {
        if (sender is not FrameworkElement element || !_viewModel.CanAdjustTarget)
        {
            return;
        }

        _targetDragElement = element;
        _targetDragReferenceArea = referenceArea;
        _targetDragLastPoint = eventArgs.GetPosition(referenceArea);
        _targetDragApply = applyDelta;
        element.CaptureMouse();
        eventArgs.Handled = true;
    }

    private void TargetDragHandle_MouseMove(object sender, MouseEventArgs eventArgs)
    {
        if (_targetDragElement is null || _targetDragReferenceArea is null || _targetDragApply is null)
        {
            return;
        }

        var referenceArea = _targetDragReferenceArea;
        if (referenceArea.ActualWidth <= 0 || referenceArea.ActualHeight <= 0)
        {
            return;
        }

        var currentPoint = eventArgs.GetPosition(referenceArea);
        var normalizedDeltaX = (currentPoint.X - _targetDragLastPoint.X) / referenceArea.ActualWidth;
        var normalizedDeltaY = (currentPoint.Y - _targetDragLastPoint.Y) / referenceArea.ActualHeight;
        _targetDragApply(normalizedDeltaX, normalizedDeltaY);
        _targetDragLastPoint = currentPoint;
    }

    private void TargetDragHandle_MouseLeftButtonUp(object sender, MouseButtonEventArgs eventArgs)
    {
        if (_targetDragElement is null)
        {
            return;
        }

        _targetDragElement.ReleaseMouseCapture();
        _targetDragElement = null;
        _targetDragReferenceArea = null;
        _targetDragApply = null;
        eventArgs.Handled = true;
    }
}
