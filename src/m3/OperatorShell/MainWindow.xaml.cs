using System.IO;
using System.Windows;
using Microsoft.Win32;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
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
    private readonly DispatcherTimer? _dualBindingHostLifetimeMonitor;
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
    private bool _shutdownStarted;
    private bool _shutdownComplete;

    // 拡大エリアの一辺と、カーソルとの間隔。MainWindow.xaml の LoupePanel と合わせる。
    private const double LoupePanelSize = 236;
    private const double LoupeCursorGap = 24;

    public MainWindow(
        DualCameraExecutionEnvironment environment = DualCameraExecutionEnvironment.TestSynthetic,
        string? dualCameraAgentExecutablePath = null,
        string? dualWpdCameraMapPath = null,
        bool captureRecoveryOnly = false,
        string? approvedCaptureProfilePath = null,
        string? dualIdentityProofPath = null)
    {
        if (captureRecoveryOnly && environment != DualCameraExecutionEnvironment.HardwareDual)
        {
            throw new ArgumentException(
                "CaptureRecoveryOnly requires the explicit HardwareDual environment.",
                nameof(captureRecoveryOnly));
        }
        if (captureRecoveryOnly &&
            (string.IsNullOrWhiteSpace(approvedCaptureProfilePath) ||
             string.IsNullOrWhiteSpace(dualIdentityProofPath)))
        {
            throw new ArgumentException(
                "CaptureRecoveryOnly requires explicit approved capture-profile and dual-identity-proof files.",
                nameof(approvedCaptureProfilePath));
        }

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
            IHardwareDualCaptureRecoveryOnlyWorkflow? captureRecoveryOnlyWorkflow = null;
            if (environment == DualCameraExecutionEnvironment.HardwareDual)
            {
                var resolvedCaptureProfilePath = captureRecoveryOnly
                    ? Path.GetFullPath(approvedCaptureProfilePath!)
                    : Path.Combine(dualProductRoot, "camera-agent", "approved-dual-capture-profile.json");
                var resolvedIdentityProofPath = captureRecoveryOnly
                    ? Path.GetFullPath(dualIdentityProofPath!)
                    : Path.Combine(dualProductRoot, "phase0", "dual-identity-proof.json");
                _dualAgentLifecycle = new DualCameraAgentLifecycle(
                    CameraAgentExecutablePolicy.Resolve(
                        AppContext.BaseDirectory,
                        dualCameraAgentExecutablePath ?? Path.Combine(
                            AppContext.BaseDirectory,
                            "A0CameraStitcher.DualCameraAgent.exe")),
                    Path.Combine(dualProductRoot, "agent-pair-journal"),
                    resolvedCaptureProfilePath,
                    resolvedIdentityProofPath,
                    dualWpdCameraMapPath ?? throw new ArgumentException(
                        "HardwareDual requires an existing WPD camera map.",
                        nameof(dualWpdCameraMapPath)));
                if (captureRecoveryOnly)
                {
                    var captureProfile = HardwareDualCaptureRecoveryOnlyProfileFile.Load(
                        resolvedCaptureProfilePath);
                    captureRecoveryOnlyWorkflow = new HardwareDualCaptureRecoveryOnlyWorkflow(
                        dualProductRoot,
                        _dualAgentLifecycle,
                        _dualAgentLifecycle,
                        captureProfile);
                }
            }
            // The product identity source remains unconfigured (HardwarePending):
            // wiring the operator's one-process binding transport does not make the
            // product capture identity Ready or permit a capture by itself.
            _viewModel = new OperatorShellViewModel(
                new SimulationFoundationService(simulatedRoot),
                DualCameraProductComposition.Create(dualProductRoot, environment, _dualAgentLifecycle),
                liveViewFramePump: environment == DualCameraExecutionEnvironment.HardwareDual
                    ? null
                    : _liveViewFramePump,
                liveViewFrameSource: environment == DualCameraExecutionEnvironment.HardwareDual
                    ? null
                    : _liveViewFrameSource,
                dualBindingTransport: _dualAgentLifecycle,
                captureRecoveryOnlyWorkflow: captureRecoveryOnlyWorkflow);
            DataContext = _viewModel;
            if (_dualAgentLifecycle is not null)
            {
                _dualBindingHostLifetimeMonitor = new DispatcherTimer(
                    DispatcherPriority.Background,
                    Dispatcher)
                {
                    Interval = TimeSpan.FromMilliseconds(500),
                };
                _dualBindingHostLifetimeMonitor.Tick += OnDualBindingHostLifetimeMonitorTick;
            }
            Loaded += OnLoaded;
            Closing += OnClosing;
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
        try
        {
            await _viewModel.InitializeAsync(_lifetime.Token);
            _dualBindingHostLifetimeMonitor?.Start();
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
        {
            // HardwareSingleCameraWindow.OnLoaded と同様、ウィンドウを閉じたことによる
            // 起動時クエリのキャンセルは無視する（issue #142 症状3）。それ以外の失敗は
            // OperatorShellViewModel.InitializeAsync 側で fail-closed に捕捉済み。
        }
    }

    // GitHub Issue #94: 以前は async void の Closed ハンドラで await していたが、最後の
    // ウィンドウの Closed 後は WPF(既定 ShutdownMode=OnLastWindowClose)が即
    // Application.Shutdown() を呼び、Dispatcher が停止して await 以降の継続が二度と
    // 実行されなかった(_dualAgentLifecycle.DisposeAsync・_liveViewFramePump.Dispose 等が
    // 飛ぶ)。Closing でいったんキャンセルしてウィンドウ(と Dispatcher)を生かしたまま
    // 非同期シャットダウンを完走させ、完了後に改めて Close() する。
    private async void OnClosing(object? sender, System.ComponentModel.CancelEventArgs eventArgs)
    {
        if (_shutdownComplete)
        {
            return;
        }
        eventArgs.Cancel = true;
        if (_shutdownStarted)
        {
            return;
        }
        _shutdownStarted = true;
        _dualBindingHostLifetimeMonitor?.Stop();
        IsEnabled = false;
        _lifetime.Cancel();
        if (_dualAgentLifecycle is not null)
        {
            // The gate releases the exclusive lease only after both the typed
            // binding cleanup acknowledgment and the child's natural exit are
            // confirmed. Any refusal, response loss or timeout leaves this
            // window open and the lease held; it never retries or kills Native.
            var outcome = await HardwareDualWindowShutdownGate.TryShutdownAsync(
                () => _viewModel.DualBinding.CancelBindingOnShutdownAsync(),
                _dualAgentLifecycle.DisposeAsync,
                () => _sessionLease?.Dispose());
            if (!outcome.Completed)
            {
                _viewModel.DualBinding.ReportShutdownBlocked(outcome.BlockingCode);
                _shutdownStarted = false;
                IsEnabled = true;
                return;
            }
        }

        _liveViewFramePump.Dispose();
        _lifetime.Dispose();
        _sessionLease?.Dispose();
        _shutdownComplete = true;
        Close();
    }

    private void OnDualBindingHostLifetimeMonitorTick(object? sender, EventArgs eventArgs) =>
        _viewModel.DualBinding.ObserveBindingHostLifetime();

    // メニューバー（issue #34）の code-behind ハンドラ。「保存先を指定」「技術情報」
    // 「バージョン」「終了」はVMへ新しいコマンド/状態を追加しない純粋なUI操作（既存の
    // 常時表示フィールドへフォーカスする・既存の読み取り専用テキストをダイアログで見せる・
    // ウィンドウを閉じる）のため、既存のドラッグハンドラと同じくcode-behindに留める。
    // 保存先はフォルダ選択ダイアログで選ぶ。手入力だと綴り違いや存在しないパスが
    // そのまま撮影可否の判定材料になり、撮り終えてから保存で失敗することになる。
    // 選んだ先がこのPC内かどうかは ViewModel 側で検証し、外部メディアや共有は拒否する。
    private void ChooseExportDirectory_Click(object sender, RoutedEventArgs eventArgs)
    {
        if (!_viewModel.CanChangeExportDirectory)
        {
            return;
        }

        var current = _viewModel.FixedLocalExportDirectory;
        var dialog = new OpenFolderDialog
        {
            Title = "撮影結果の保存先を選択（このPC内のフォルダのみ）",
            Multiselect = false,
        };
        if (!string.IsNullOrWhiteSpace(current) && Directory.Exists(current))
        {
            dialog.InitialDirectory = current;
        }

        if (dialog.ShowDialog(this) == true)
        {
            _viewModel.ChangeExportDirectory(dialog.FolderName);
        }
    }

    private void ShowTechnicalDetail_Click(object sender, RoutedEventArgs eventArgs) =>
        MessageBox.Show(this, _viewModel.TechnicalDetail, "技術情報（error code・ログ位置）", MessageBoxButton.OK, MessageBoxImage.Information);

    private void ShowVersion_Click(object sender, RoutedEventArgs eventArgs) =>
        MessageBox.Show(this, OperatorShellViewModel.AppVersionText, "バージョン", MessageBoxButton.OK, MessageBoxImage.Information);

    private void ExitMenuItem_Click(object sender, RoutedEventArgs eventArgs) => Close();

    // タイトルバーは 1920×1080 キャンバスの中にあり、ウィンドウ縮小率に応じて実際の高さが変わる。
    // OS の caption 判定（WindowChrome.CaptionHeight）は物理座標で効くため実領域とずれる。
    // そこで CaptionHeight=0 とし、移動・最大化の操作をここで受ける。
    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs eventArgs)
    {
        if (eventArgs.ChangedButton != MouseButton.Left)
        {
            return;
        }

        if (eventArgs.ClickCount == 2)
        {
            ToggleMaximizedState();
            return;
        }

        if (WindowState == WindowState.Maximized)
        {
            // 最大化のままではドラッグで動かせないため、掴んだ位置の比率を保ったまま復元する。
            // PointToScreen はデバイスピクセルを返すのに Left/Top は DIP なので、変換を挟まないと
            // 150% 表示などで復元位置がカーソルから離れていく。復元後の寸法は RestoreBounds で取る
            // （この時点の Width/Height はまだ最大化時の値のことがある）。
            var grabPoint = eventArgs.GetPosition(this);
            var grabRatioX = grabPoint.X / Math.Max(1.0, ActualWidth);
            var grabRatioY = grabPoint.Y / Math.Max(1.0, ActualHeight);
            var deviceToDip = PresentationSource.FromVisual(this)?.CompositionTarget?.TransformFromDevice
                ?? Matrix.Identity;
            var cursorOnScreen = deviceToDip.Transform(PointToScreen(grabPoint));
            var restored = RestoreBounds;
            var restoredWidth = restored.Width > 0 ? restored.Width : Width;
            var restoredHeight = restored.Height > 0 ? restored.Height : Height;

            WindowState = WindowState.Normal;
            Left = cursorOnScreen.X - (restoredWidth * grabRatioX);
            Top = cursorOnScreen.Y - (restoredHeight * grabRatioY);
        }

        DragMove();
    }

    private void ToggleMaximizedState() =>
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void MinimizeWindow_Click(object sender, RoutedEventArgs eventArgs) =>
        WindowState = WindowState.Minimized;

    private void MaximizeWindow_Click(object sender, RoutedEventArgs eventArgs) => ToggleMaximizedState();

    private void CloseWindow_Click(object sender, RoutedEventArgs eventArgs) => Close();

    // A / B で表示カメラを切り替え、F でAFを実行する（docs/OPERATOR_UI_SPEC.md キー操作）。
    // Window.InputBindings に置くと修飾キーなしの1文字が文字入力より先に走り、保存先へ
    // "F:\..." と打っただけでAFが動いてしまう。入力欄にフォーカスがある間は何もしない。
    private void Window_PreviewKeyDown(object sender, KeyEventArgs eventArgs)
    {
        if (eventArgs.Handled ||
            eventArgs.KeyboardDevice.Modifiers != ModifierKeys.None ||
            Keyboard.FocusedElement is TextBoxBase or PasswordBox)
        {
            return;
        }

        switch (eventArgs.Key)
        {
            case Key.A:
                _viewModel.SelectStageCamera("CAM-A");
                break;
            case Key.B:
                _viewModel.SelectStageCamera("CAM-B");
                break;
            case Key.F:
                if (_viewModel.AutoFocusCommand.CanExecute(null))
                {
                    _viewModel.AutoFocusCommand.Execute(null);
                }

                break;
            default:
                return;
        }

        eventArgs.Handled = true;
    }

    private void ToggleTechnicalDetail_Click(object sender, RoutedEventArgs eventArgs)
    {
        var showing = TechnicalDetailRow.Visibility != Visibility.Visible;
        TechnicalDetailRow.Visibility = showing ? Visibility.Visible : Visibility.Collapsed;
        TechnicalDetailToggle.Content = showing ? "▾ 詳細情報" : "▸ 詳細情報";
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

    // 拡大エリアはポインタが指した場所を映す。ステージ上の移動をそのまま ViewModel の
    // 切り出し中心へ渡し、パネル自体はカーソルの脇へ寄せて置く（カーソル直下に重ねると
    // 見たい場所をルーペが隠す）。ドラッグ中は中心を動かさない — 拡大像が流れると
    // ターゲットをどこへ動かしているのか分からなくなるため。
    private void UpdateLoupeFollow(MouseEventArgs eventArgs)
    {
        if (_targetDragElement is not null || StageDisplayArea.ActualWidth <= 0 || StageDisplayArea.ActualHeight <= 0)
        {
            return;
        }

        var point = eventArgs.GetPosition(StageDisplayArea);
        var width = StageDisplayArea.ActualWidth;
        var height = StageDisplayArea.ActualHeight;
        if (point.X < 0 || point.Y < 0 || point.X > width || point.Y > height)
        {
            _viewModel.ClearPointerPosition();
            return;
        }

        _viewModel.UpdatePointerPosition(point.X / width, point.Y / height);

        var left = point.X + LoupeCursorGap;
        if (left + LoupePanelSize > width)
        {
            left = point.X - LoupeCursorGap - LoupePanelSize;
        }

        var top = point.Y + LoupeCursorGap;
        if (top + LoupePanelSize > height)
        {
            top = point.Y - LoupeCursorGap - LoupePanelSize;
        }

        LoupePanel.Margin = new Thickness(
            Math.Clamp(left, 0, Math.Max(0, width - LoupePanelSize)),
            Math.Clamp(top, 0, Math.Max(0, height - LoupePanelSize)),
            0,
            0);
    }

    private void StageHandle_MouseLeave(object sender, MouseEventArgs eventArgs)
    {
        if (_targetDragElement is not null)
        {
            return;
        }

        // ステージ内の別要素へ移っただけの離脱では畳まない。実際にステージの外へ
        // 出たときだけ拡大エリアを消す。
        var point = eventArgs.GetPosition(StageDisplayArea);
        if (point.X < 0 || point.Y < 0 || point.X > StageDisplayArea.ActualWidth || point.Y > StageDisplayArea.ActualHeight)
        {
            _viewModel.ClearPointerPosition();
        }
    }

    private void TargetDragHandle_MouseMove(object sender, MouseEventArgs eventArgs)
    {
        UpdateLoupeFollow(eventArgs);

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
