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
            Closed += OnClosed;
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

    private async void OnClosed(object? sender, EventArgs eventArgs)
    {
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
        }
    }
}
