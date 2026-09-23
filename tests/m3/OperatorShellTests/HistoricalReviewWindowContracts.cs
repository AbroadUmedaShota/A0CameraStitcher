using System.IO;
using System.Security.Cryptography;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.OperatorShell;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

internal static class HistoricalReviewWindowContracts
{
    internal static async Task RunAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "A0-HistoryWindow-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        var shell = new OperatorShellViewModel(new SimulationFoundationService(Path.Combine(root, "journals")));
        var brokenReviewRoot = Path.Combine(root, "broken-reviews");
        Directory.CreateDirectory(brokenReviewRoot);
        await File.WriteAllTextAsync(Path.Combine(brokenReviewRoot, "incomplete.partial"), "synthetic-incomplete");
        var failedShell = new OperatorShellViewModel(new SimulationFoundationService(Path.Combine(root, "failed-journals")), null,
            operatorReviewStore: new FileOperatorReviewStore(brokenReviewRoot));
        await failedShell.InitializeAsync(CancellationToken.None);
        Require(failedShell.UiState == OperatorUiState.FailedPartial && !failedShell.CanOpenHistoricalReview &&
            !failedShell.TryBeginHistoricalReview(), "Initialization failure allowed history.");
        Require(!shell.CanOpenHistoricalReview && !shell.TryBeginHistoricalReview(), "Initialization was bypassed.");
        await shell.InitializeAsync(CancellationToken.None);
        var readOnlyHistory = new HistoricalReviewWindow(root,
            new M2OfflineStitcherProcessAdapter(Environment.ProcessPath!), "Simulated", readOnly: true);
        var acceptButton = readOnlyHistory.FindName("AcceptButton") as Button;
        Require(acceptButton is { Visibility: Visibility.Collapsed, IsEnabled: false } &&
            readOnlyHistory.Title.Contains("採用不可", StringComparison.Ordinal),
            "Read-only history exposed the acceptance action.");
        readOnlyHistory.Close();
        Require(shell.CanOpenHistoricalReview, "Idle initialized shell cannot browse history.");
        var state = shell.UiState;
        var starts = shell.TransactionStartCount;
        Require(shell.TryBeginHistoricalReview(), "History reservation failed.");
        Require(shell.IsBusy && !shell.CanCapture && !shell.CanUseLiveView && !shell.CanAcceptReview &&
            !shell.TryBeginHistoricalReview(), "History did not block competing actions.");
        shell.EndHistoricalReview();
        Require(!shell.IsBusy && shell.UiState == state && shell.TransactionStartCount == starts,
            "History changed current result or started capture.");
        shell.AcceptSafetyCommand.Execute(null);
        shell.ToggleLiveViewCommand.Execute(null);
        Require(shell.IsLiveViewActive && !shell.CanOpenHistoricalReview && !shell.TryBeginHistoricalReview(),
            "Live View did not block history.");
        shell.ToggleLiveViewCommand.Execute(null);
        Require(shell.CanOpenHistoricalReview, "Stopped preview did not restore history gate.");
        shell.DualBinding.ReportShutdownBlocked("test-unknown-close");
        Require(!shell.CanOpenHistoricalReview && !shell.TryBeginHistoricalReview(), "Unknown camera close allowed history.");

        var store = new FileOperatorReviewStore(Path.Combine(root, "reviews"));
        for (var index = 0; index < 27; index++)
        {
            var record = new OperatorReviewRecord { ResultId = Guid.NewGuid().ToString("N"),
                TransactionId = Guid.NewGuid().ToString("N"), ReviewKind = index == 0 ? "Product" : "Simulated",
                State = "Pending", UpdatedAtUtc = DateTimeOffset.UtcNow.AddMinutes(index) };
            await store.SaveAsync(record);
            if (index >= 2) await store.SaveAsync(record with { State = "Accepted" });
        }
        var history = new HistoricalReviewViewModel();
        var page = await store.QueryReadOnlyAsync(0, 25);
        history.ApplyPage(page, 0, "Simulated");
        Require(history.Items.Count == 0 && history.TryGetNextOffset(out var next) && next == 25,
            "An accepted-only page hid older Pending records.");
        history.IsBusy = true;
        Require(!history.CanNext && !history.TryGetNextOffset(out _) && !history.CanOpen,
            "Busy page permitted another operation.");
        history.IsBusy = false;
        history.ApplyPage(await store.QueryReadOnlyAsync(25, 25), 25, "Simulated");
        history.CommitNextNavigation(0);
        Require(history.Items.Count == 1 && history.Items[0].Record.ReviewKind == "Simulated" &&
            history.TryGetPreviousOffset(out var previous) && previous == 0 && !history.CanNext,
            "Previous page offset or mode isolation is incorrect.");
        history.Selected = history.Items[0];
        Require(history.CanOpen, "Explicit selection did not enable verification.");
        var pending = history.Selected.Record;
        var shown = new HistoricalReviewArtifacts(Guid.ParseExact(pending.ResultId, "N"),
            Guid.ParseExact(pending.TransactionId, "N"), "Simulated", [], "synthetic-only", new string('a', 64),
            16, 8, "synthetic", "1", new string('b', 64));
        Require(!history.CanAccept, "Unviewed result was confirmable.");
        history.RecordViewed(pending, shown, 0);
        Require(history.CanAccept && !history.IsBusy, "Verified review did not permit a single explicit acceptance action.");
        history.RecordViewed(pending, shown, 1);
        var changed = shown with { ManifestSha256 = new string('c', 64) };
        history.RecordViewed(pending, changed, 2);
        Require(history.VerificationProgress.Contains("1/3", StringComparison.Ordinal), "Changed result retained previous image view counts.");
        history.RecordViewed(pending, changed, 0);
        history.RecordViewed(pending, changed, 1);
        Require(history.CanAccept && !history.IsBusy, "Viewing started acceptance automatically.");
        Require(history.TryBeginAcceptance(out var selectedPending, out var selectedShown) && selectedPending == pending && selectedShown == changed,
            "Explicit acceptance did not retain the selected snapshot.");
        Require(!history.TryBeginAcceptance(out _, out _) && history.IsBusy, "Acceptance allowed concurrent submission.");
        history.IsBusy = false; // Ambiguous failure: no durable success was returned.
        Require(!history.CanAccept && !history.TryBeginAcceptance(out _, out _), "Ambiguous acceptance was resubmitted.");
        // A failed page load does not commit navigation or lose the return offset.
        Require(history.TryGetPreviousOffset(out previous) && previous == 0, "Reading navigation consumed its offset.");
        history.ApplyPage(page, 0, "Simulated");
        history.CommitPreviousNavigation();
        Require(!history.CanPrevious && !history.CanOpen && history.Selected is null,
            "Page change retained an obsolete selection.");
        Require((await store.QueryReadOnlyAsync(25, 25)).Records.All(record => record.State == "Pending"),
            "Viewing metadata accepted a result.");
        var historyShell = new OperatorShellViewModel(new SimulationFoundationService(Path.Combine(root, "history-journals")),
            null, operatorReviewStore: store);
        await historyShell.InitializeAsync(CancellationToken.None);
        var historyShellState = historyShell.UiState;
        historyShell.ObserveHistoricalAcceptance(pending with { State = "Accepted" });
        Require(historyShell.ReviewStatusText.Contains("残り 1 件", StringComparison.Ordinal) &&
            !historyShell.ReviewStatusText.Contains(pending.ResultId, StringComparison.Ordinal) &&
            historyShell.TransactionStartCount == 0 && historyShell.UiState == historyShellState,
            "Historical acceptance left stale Pending status or started capture.");
        history.ApplyPage(new ReviewMetadataPage([pending], null, 1), 0, "Simulated");
        history.Selected = history.Items[0];
        foreach (var choice in new[] { 0, 1, 2 }) history.RecordViewed(pending, shown, choice);
        Require(history.TryBeginAcceptance(out _, out _), "Fresh explicit review could not begin acceptance.");
        history.RecordAcceptance(pending with { State = "Accepted" }); // UI-only durable-success response fixture.
        history.IsBusy = false;
        Require(history.Items.Count == 0 && history.Selected is null && !history.CanAccept,
            "Accepted result remained eligible for another acceptance.");

        var imagePath = Path.Combine(root, "synthetic.jpg");
        var bitmap = BitmapSource.Create(2, 2, 96, 96, PixelFormats.Bgr24, null,
            new byte[] { 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255 }, 6);
        var encoder = new JpegBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using (var output = File.Create(imagePath)) encoder.Save(output);
        var original = File.ReadAllBytes(imagePath);
        var expected = Convert.ToHexString(SHA256.HashData(original));
        var viewer = new ReviewImageWindow(imagePath, null, expected);
        viewer.Close();
        try
        {
            var rejected = new ReviewImageWindow(imagePath, null, new string('0', 64));
            rejected.Close();
            throw new Exception("Changed image was displayed.");
        }
        catch (InvalidDataException) { }
        Require(original.SequenceEqual(File.ReadAllBytes(imagePath)), "Viewer modified the image.");
        // Synthetic fixtures retained for diagnosis. No product storage or hardware was touched.
    }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }
}
