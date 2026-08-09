using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

var failures = new List<string>();
try
{
    await LiveViewStopFailureWorkflowAsync();
    Console.WriteLine("PASS live view stop failure survives restart and rejects duplicate start");
}
catch (Exception exception)
{
    failures.Add("live view stop failure survives restart and rejects duplicate start");
    Console.Error.WriteLine($"FAIL live view stop failure survives restart and rejects duplicate start: {exception}");
}

Console.WriteLine($"Operator shell tests: {1 - failures.Count}/1 passed.");
return failures.Count == 0 ? 0 : 1;

static async Task LiveViewStopFailureWorkflowAsync()
{
    var root = Path.Combine(
        Path.GetTempPath(),
        "A0CameraStitcher-M3-OperatorShellTests",
        Guid.NewGuid().ToString("N"));
    Directory.CreateDirectory(root);
    try
    {
        var viewModel = new OperatorShellViewModel(new SimulationFoundationService(root));
        await viewModel.InitializeAsync(CancellationToken.None);
        viewModel.AcceptSafetyCommand.Execute(null);
        Check.True(viewModel.CanCapture, "The simulated shell must be ready before the diagnostic starts.");

        viewModel.SelectedDiagnosticScenario = "Live View停止失敗";
        viewModel.DiagnosticCommand.Execute(null);
        viewModel.DiagnosticCommand.Execute(null);
        await WaitUntilAsync(
            () => viewModel.TransactionStartCount == 1 && !viewModel.IsBusy,
            "The durable failure workflow did not finish.");

        Check.Equal(1, viewModel.TransactionStartCount);
        Check.Equal(OperatorUiState.FailedPartial, viewModel.UiState);
        Check.True(viewModel.TechnicalDetail.Contains("LiveViewStopFailed", StringComparison.Ordinal), "The terminal reason must be visible.");
        Check.True(viewModel.TechnicalDetail.Contains("capture calls: 0", StringComparison.Ordinal), "Zero capture calls must be visible.");
        Check.True(viewModel.TechnicalDetail.Contains("automatic retry count: 0", StringComparison.Ordinal), "Zero retries must be visible.");
        Check.False(viewModel.CanCapture, "A failed transaction must block another capture until preparation.");
        Check.True(viewModel.CanPrepareNewCapture, "The operator must be able to explicitly prepare a new transaction.");
        Check.Equal(1, Directory.EnumerateDirectories(root).Count());

        var transactionId = Guid.ParseExact(viewModel.LastTransactionId, "N");
        viewModel.DiagnosticCommand.Execute(null);
        await Task.Delay(100);
        Check.Equal(1, viewModel.TransactionStartCount);

        var restarted = new OperatorShellViewModel(new SimulationFoundationService(root));
        await restarted.InitializeAsync(CancellationToken.None);
        Check.Equal(OperatorUiState.FailedPartial, restarted.UiState);
        Check.Equal(transactionId.ToString("N"), restarted.LastTransactionId);
        Check.True(restarted.TechnicalDetail.Contains("LiveViewStopFailed", StringComparison.Ordinal), "Restart must rediscover the same terminal reason.");
        Check.False(restarted.CanCapture, "Restart must not resume or recapture the failed transaction.");

        restarted.AcceptSafetyCommand.Execute(null);
        Check.False(restarted.CanCapture, "Safety acknowledgment must not bypass explicit new-capture preparation.");
        restarted.PrepareNewCaptureCommand.Execute(null);
        await WaitUntilAsync(() => !restarted.IsBusy, "Preparing a new transaction did not finish.");
        Check.True(restarted.CanCapture, "Only explicit preparation may allow a new transaction.");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static async Task WaitUntilAsync(Func<bool> predicate, string message)
{
    var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
    while (!predicate())
    {
        if (DateTime.UtcNow >= deadline)
        {
            throw new TimeoutException(message);
        }
        await Task.Delay(20);
    }
}

static class Check
{
    public static void True(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    public static void False(bool condition, string message) => True(!condition, message);

    public static void Equal<T>(T expected, T actual)
        where T : notnull
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
        {
            throw new InvalidOperationException($"Expected {expected}, got {actual}.");
        }
    }
}
