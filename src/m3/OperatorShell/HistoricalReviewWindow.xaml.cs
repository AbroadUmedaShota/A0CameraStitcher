using System.ComponentModel;
using System.IO;
using System.Text.Json;
using System.Windows;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

namespace A0CameraStitcher.M3.OperatorShell;

public partial class HistoricalReviewWindow : Window
{
    private const int PageSize = 25;
    private readonly FileOperatorReviewStore _store;
    private readonly M2OfflineStitcherProcessAdapter _adapter;
    private readonly string _productRoot;
    private readonly string _expectedReviewKind;
    private readonly HistoricalReviewViewModel _viewModel = new();
    private readonly CancellationTokenSource _lifetime = new();
    private bool _closed;
    private bool _lifetimeDisposed;

    internal HistoricalReviewWindow(string productRoot, M2OfflineStitcherProcessAdapter adapter, string expectedReviewKind)
    {
        if (string.IsNullOrWhiteSpace(productRoot) || adapter is null || expectedReviewKind is not ("Product" or "Simulated"))
            throw new ArgumentException("Historical review context is invalid.");
        _productRoot = Path.GetFullPath(productRoot);
        _adapter = adapter;
        _expectedReviewKind = expectedReviewKind;
        _store = new FileOperatorReviewStore(Path.Combine(_productRoot, "operator-review"));
        InitializeComponent();
        Title = expectedReviewKind == "Simulated" ? "模擬結果の履歴（SIMULATED）" : "保存済み合成結果の履歴";
        ContextLabel.Text = (expectedReviewKind == "Simulated" ? "SIMULATED：実機の撮影・品質受入ではありません。\n" : "品質合格・採用を自動判定しません。\n") + ContextLabel.Text;
        DataContext = _viewModel;
        Loaded += OnLoaded;
        Closed += (_, _) =>
        {
            _closed = true;
            _lifetime.Cancel();
            DisposeLifetimeIfClosed();
        };
    }

    private async void OnLoaded(object sender, RoutedEventArgs e) => await LoadPageAsync(0);

    private async Task<bool> LoadPageAsync(int offset)
    {
        if (_viewModel.IsBusy || _closed || _lifetime.IsCancellationRequested) return false;
        _viewModel.IsBusy = true;
        try
        {
            var page = await _store.QueryReadOnlyAsync(offset, PageSize, _lifetime.Token);
            if (_closed || _lifetime.IsCancellationRequested) return false;
            _viewModel.ApplyPage(page, offset, _expectedReviewKind);
            _viewModel.Status = _viewModel.Items.Count == 0
                ? "このページに開けるPending結果はありません。採用は未対応です。"
                : $"Pending結果 {_viewModel.Items.Count}件を表示しています。採用は未対応です。";
            return true;
        }
        catch (OperationCanceledException)
        {
            if (!_closed) _viewModel.Status = "履歴の読取りを中止しました。自動再試行はしません。";
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException or
            InvalidDataException or InvalidOperationException or NotSupportedException or JsonException or Win32Exception)
        {
            _viewModel.Status = "確認記録を安全に読み取れないため、開きませんでした。";
        }
        finally
        {
            _viewModel.IsBusy = false;
            DisposeLifetimeIfClosed();
        }
        return false;
    }

    private async void OnPrevious(object sender, RoutedEventArgs e)
    {
        if (_viewModel.IsBusy || _lifetime.IsCancellationRequested || !_viewModel.TryGetPreviousOffset(out var previous)) return;
        if (await LoadPageAsync(previous)) _viewModel.CommitPreviousNavigation();
    }

    private async void OnNext(object sender, RoutedEventArgs e)
    {
        if (_viewModel.IsBusy || _lifetime.IsCancellationRequested || !_viewModel.TryGetNextOffset(out var next)) return;
        var current = _viewModel.Offset;
        if (await LoadPageAsync(next)) _viewModel.CommitNextNavigation(current);
    }

    private async void OnOpen(object sender, RoutedEventArgs e)
    {
        if (_viewModel.IsBusy || _lifetime.IsCancellationRequested || _viewModel.Selected is not { } snapshot) return;
        var choice = ImageChoice.SelectedIndex;
        _viewModel.IsBusy = true;
        _viewModel.Status = "選択した保存結果を再検証しています…";
        try
        {
            var artifacts = await _adapter.VerifyHistoricalReviewAsync(_productRoot, snapshot.Record, _lifetime.Token);
            _lifetime.Token.ThrowIfCancellationRequested();
            if (_closed || !IsLoaded) return;
            var path = choice switch
            {
                0 => artifacts.StitchedPath,
                1 => artifacts.Originals.Single(original => original.Alias == "CAM-A").Path,
                2 => artifacts.Originals.Single(original => original.Alias == "CAM-B").Path,
                _ => throw new InvalidOperationException(),
            };
            var hash = choice == 0 ? artifacts.StitchedSha256 : artifacts.Originals.Single(original => original.Path == path).Sha256;
            var seam = choice == 0 ? StitchSeamNavigationManifestReader.TryReadForStitchedOutput(path) : null;
            new ReviewImageWindow(path, seam, hash)
            {
                Owner = this,
                Title = _expectedReviewKind == "Simulated" ? "模擬結果の詳細確認（SIMULATED・閲覧のみ）" : "保存結果の詳細確認（閲覧のみ）",
            }.ShowDialog();
            _viewModel.Status = "表示のみで開きました。採用は未対応です。";
        }
        catch (OperationCanceledException)
        {
            if (!_closed) _viewModel.Status = "再検証が中断または時間切れになりました。自動再試行はしません。";
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException or
            InvalidDataException or InvalidOperationException or NotSupportedException or JsonException or Win32Exception or FormatException)
        {
            _viewModel.Status = "保存結果を安全に再検証できないため、開きませんでした。";
        }
        finally
        {
            _viewModel.IsBusy = false;
            DisposeLifetimeIfClosed();
        }
    }

    private void OnClose(object sender, RoutedEventArgs e) => Close();

    private void DisposeLifetimeIfClosed()
    {
        if (_closed && !_viewModel.IsBusy && !_lifetimeDisposed)
        {
            _lifetimeDisposed = true;
            _lifetime.Dispose();
        }
    }
}
