using System.IO;
using System.Windows;
using Microsoft.Win32;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

namespace A0CameraStitcher.M3.OperatorShell;

public partial class HardwareSingleCameraWindow : Window
{
    private readonly CancellationTokenSource _lifetime = new();
    private readonly HardwareSingleAppSessionLease _sessionLease;
    private readonly PersistentHardwareCameraAgentOperations _operations;
    private readonly HardwareSingleCameraViewModel _viewModel;
    private bool _shutdownStarted;
    private bool _shutdownComplete;

    public HardwareSingleCameraWindow(string cameraAgentExecutablePath)
    {
        _sessionLease = HardwareSingleAppSessionLease.Acquire();
        try
        {
            InitializeComponent();
            var storagePaths = HardwareSingleStoragePaths.Resolve(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData));
            var preferencesStore = new HardwareSinglePreferencesStore(storagePaths.PreferencesPath);
            var profileStore = new HardwareSingleCaptureProfileStore(storagePaths.CaptureProfilePath);
            _operations = new PersistentHardwareCameraAgentOperations(
                cameraAgentExecutablePath,
                storagePaths.AgentArtifactsRoot,
                storagePaths.CaptureProfilePath,
                storagePaths.SingleIdentityV3Path);
            _viewModel = new HardwareSingleCameraViewModel(
                _operations,
                new HardwareSingleAppStateStore(storagePaths.StateDirectory),
                new HardwareOriginalExporter(storagePaths.DefaultExportDirectory),
                preferencesStore,
                profileStore);
            DataContext = _viewModel;
            Loaded += OnLoaded;
            Closing += OnClosing;
        }
        catch
        {
            _sessionLease.Dispose();
            throw;
        }
    }

    private async void OnChooseExportDirectory(object sender, RoutedEventArgs eventArgs)
    {
        var dialog = new OpenFolderDialog
        {
            Title = "検証済みSingleCamera原画像の保存先を選択",
            InitialDirectory = _viewModel.ExportDirectory,
            Multiselect = false,
        };
        if (dialog.ShowDialog(this) == true)
        {
            await _viewModel.ChangeExportDirectoryAsync(dialog.FolderName, _lifetime.Token);
        }
    }

    private async void OnLoaded(object sender, RoutedEventArgs eventArgs)
    {
        Loaded -= OnLoaded;
        try
        {
            await _viewModel.InitializeAsync(_lifetime.Token);
        }
        catch (OperationCanceledException) when (_lifetime.IsCancellationRequested)
        {
            // Closing the window may cancel a read-only startup query. A durable
            // pending transaction remains intact and will be queried next launch.
        }
    }

    // GitHub Issue #94: 以前は async void の Closed ハンドラで await していたが、最後の
    // ウィンドウの Closed 後は WPF(既定 ShutdownMode=OnLastWindowClose)が即
    // Application.Shutdown() を呼び、Dispatcher が停止して await 以降の継続が二度と
    // 実行されなかった(Live View の Close 送信・_operations.DisposeAsync・各 Dispose が
    // 飛び、ネイティブ Agent がカメラを最大寿命まで保持)。Closing でいったんキャンセルして
    // ウィンドウ(と Dispatcher)を生かしたまま非同期シャットダウンを完走させ、完了後に
    // 改めて Close() する。
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
        IsEnabled = false;
        _lifetime.Cancel();
        try
        {
            try
            {
                await _viewModel.ShutdownAsync();
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                // Closing remains fail-closed. The native heartbeat/max-lifetime
                // guards own cleanup if an orderly stop cannot be confirmed.
            }
            try
            {
                await _operations.DisposeAsync();
            }
            catch (Exception exception) when (exception is not OutOfMemoryException)
            {
                // Never force-kill an agent whose capture dispatch state may be
                // ambiguous. Durable recovery resolves it on the next launch.
            }
        }
        finally
        {
            _lifetime.Dispose();
            _viewModel.Dispose();
            _sessionLease.Dispose();
            _shutdownComplete = true;
            Close();
        }
    }
}
