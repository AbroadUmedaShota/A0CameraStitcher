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
    private readonly bool _readOnly;
    private readonly HistoricalReviewViewModel _viewModel = new();
    private readonly CancellationTokenSource _lifetime = new();
    private bool _closed;
    private bool _lifetimeDisposed;
    private bool _acceptanceInProgress;
    private bool _closeRequested;
    private readonly List<OperatorReviewRecord> _acceptedReviews = [];
    internal IReadOnlyList<OperatorReviewRecord> AcceptedReviews => _acceptedReviews;

    internal HistoricalReviewWindow(string productRoot, M2OfflineStitcherProcessAdapter adapter, string expectedReviewKind, bool readOnly = false)
    {
        if (string.IsNullOrWhiteSpace(productRoot) || adapter is null || expectedReviewKind is not ("Product" or "Simulated"))
            throw new ArgumentException("Historical review context is invalid.");
        _productRoot = Path.GetFullPath(productRoot);
        _adapter = adapter;
        _expectedReviewKind = expectedReviewKind;
        _readOnly = readOnly;
        _store = new FileOperatorReviewStore(Path.Combine(_productRoot, "operator-review"));
        InitializeComponent();
        Title = readOnly ? "保存済み結果の照会（採用不可）" :
            expectedReviewKind == "Simulated" ? "模擬結果の履歴（SIMULATED）" : "保存済み合成結果の履歴";
        ContextLabel.Text = (expectedReviewKind == "Simulated" ? "SIMULATED：実機の撮影・品質受入ではありません。\n" : "品質合格・採用を自動判定しません。\n") + ContextLabel.Text;
        if (readOnly)
        {
            ContextLabel.Text = "照会専用：この画面から採用・撮影・再合成はできません。\n" + ContextLabel.Text;
            AcceptButton.Visibility = Visibility.Collapsed;
            AcceptButton.IsEnabled = false;
        }
        DataContext = _viewModel;
        Loaded += OnLoaded;
        Closing += (_, args) =>
        {
            if (!_acceptanceInProgress) return;
            args.Cancel = true;
            _closeRequested = true;
            // Do not interrupt a metadata publication just because the operator
            // closes the window. Keep the shell gate until its outcome is known.
            _viewModel.Status = "採用記録の保存状態を確認してから閉じます。自動再送はしません。";
        };
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
                ? "このページに開ける未採用結果はありません。"
                : $"未採用結果 {_viewModel.Items.Count}件。選択して合成・左右原画像を詳しく確認してください。";
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
            if (!_closed) _viewModel.RecordViewed(snapshot.Record, artifacts, choice);
            _viewModel.Status = "画像を表示しました。詳細確認と明示操作が済むまで採用は記録しません。";
        }
        catch (OperationCanceledException)
        {
            _viewModel.ResetVerification();
            if (!_closed) _viewModel.Status = "再検証が中断または時間切れになりました。自動再試行はしません。";
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException or
            InvalidDataException or InvalidOperationException or NotSupportedException or JsonException or Win32Exception or FormatException)
        {
            _viewModel.ResetVerification();
            _viewModel.Status = "保存結果を安全に再検証できないため、開きませんでした。";
        }
        finally
        {
            _viewModel.IsBusy = false;
            DisposeLifetimeIfClosed();
        }
    }

    private async void OnAccept(object sender, RoutedEventArgs e)
    {
        if (_readOnly || _closed || _lifetime.IsCancellationRequested ||
            !_viewModel.TryBeginAcceptance(out var pending, out var reviewed)) return;
        _acceptanceInProgress = true;
        var acceptanceSucceeded = false;
        _viewModel.Status = "確認した画像と記録を再検証し、採用を保存しています…";
        try
        {
            var accepted = await _adapter.AcceptHistoricalReviewAsync(_productRoot, pending!, reviewed!, _lifetime.Token);
            _acceptedReviews.Add(accepted);
            _viewModel.RecordAcceptance(accepted);
            acceptanceSucceeded = true;
            _viewModel.Status = "採用を保存・再読取り確認しました。撮影は開始しません。閉じて次の原稿を準備できます。";
        }
        catch (OperationCanceledException)
        {
            _viewModel.Status = "採用処理を中止しました。再送せず、履歴を読み直して保存状態を確認してください。";
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException or
            InvalidDataException or InvalidOperationException or NotSupportedException or JsonException or Win32Exception or FormatException)
        {
            _viewModel.Status = "採用の保存結果を確認できません。自動再送せず、履歴を読み直してください。";
        }
        finally
        {
            _acceptanceInProgress = false;
            _viewModel.IsBusy = false;
            if (_closeRequested && acceptanceSucceeded) Close();
            else _closeRequested = false; // Keep failure/unknown visible for an explicit next decision.
        }
    }

    private async void OnRefresh(object sender, RoutedEventArgs e) => await LoadPageAsync(_viewModel.Offset);

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
