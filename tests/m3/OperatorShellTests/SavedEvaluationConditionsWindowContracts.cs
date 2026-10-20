using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.OperatorShell;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

internal static class SavedEvaluationConditionsWindowContracts
{
    internal static async Task RunAsync(string? actualConditions = null)
    {
        var directory = Path.Combine(Path.GetTempPath(), "A0-ConditionsWindow-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var reference = new { relativePath = "draft.json", sha256 = new string('a', 64), encodedSizeBytes = 128 };
        var valid = JsonSerializer.Serialize(new
        {
            schemaVersion = "a0.saved-review-evaluation-conditions.v1", productApproved = false, quality = "not-evaluated",
            pairId = "synthetic-only", reviewReportSha256 = new string('b', 64), reviewImageSha256 = new string('c', 64),
            inputs = new[] { new { cameraAlias = "CAM-A", sha256 = new string('d', 64), encodedSizeBytes = 128 },
                new { cameraAlias = "CAM-B", sha256 = new string('e', 64), encodedSizeBytes = 256 } },
            profile = reference, correction = reference, color = (object?)null,
            raster = new { leftUm = 565000, topUm = 400000, rightUm = 575000, bottomUm = 408000, dpi = 100, widthPixels = 40, heightPixels = 32, resampling = "bilinear" }
        });
        var file = Path.Combine(directory, "chosen.json");
        File.WriteAllText(file, valid, new UTF8Encoding(false));
        var summary = SavedEvaluationConditionsSummary.Parse(Encoding.UTF8.GetBytes(valid));
        Require(summary.Description.Contains("100 DPI", StringComparison.Ordinal) && summary.Description.Contains("40 × 32", StringComparison.Ordinal), "Chosen raster was not displayed.");
        foreach (var bad in new[]
        {
            valid.Replace("\"dpi\":100", "\"dpi\":100.0", StringComparison.Ordinal),
            valid.Replace("\"dpi\":100", "\"dpi\":100,\"dpi\":100", StringComparison.Ordinal),
            valid.Replace("\"dpi\":100", "\"dpi\":0", StringComparison.Ordinal),
            valid.Replace("\"widthPixels\":40", "\"widthPixels\":41", StringComparison.Ordinal),
            valid.Replace("\"productApproved\":false", "\"productApproved\":true", StringComparison.Ordinal),
            valid.Replace("conditions.v1", "conditions.v2", StringComparison.Ordinal),
            valid.Replace("draft.json", "../draft.json", StringComparison.Ordinal),
            valid.Replace("draft.json", "NUL.json", StringComparison.Ordinal),
            valid.Replace("\"dpi\":100", "\"dpi\":100,\"command\":\"ignored.exe\"", StringComparison.Ordinal),
            valid.Replace(new string('b', 64), new string('b', 64) + "\\n", StringComparison.Ordinal)
        })
        {
            try { SavedEvaluationConditionsSummary.Parse(Encoding.UTF8.GetBytes(bad)); throw new Exception("Invalid conditions were accepted."); }
            catch (InvalidDataException) { }
        }
        var model = new SavedEvaluationConditionsViewModel();
        Require(model.LoadFile(file) && model.Summary.Contains("100 DPI", StringComparison.Ordinal), "Explicit selection did not load.");
        var before = File.ReadAllBytes(file);
        var malformed = Path.Combine(directory, "broken.json"); File.WriteAllText(malformed, "{}");
        Require(!model.LoadFile(malformed) && model.Summary.Length == 0 && model.SelectedFile == "未選択", "Failed selection retained stale successful contents.");
        Require(!model.LoadFile(Path.Combine(directory, "absent.json")) && model.Summary.Length == 0, "Missing selection retained prior data.");
        Require(!model.LoadFile(@"\\not-opened.invalid\share\conditions.json") && model.Summary.Length == 0, "UNC selection was not rejected before file access.");
        Require(!model.LoadFile(file + ":stream") && model.Summary.Length == 0, "Alternate data stream selection was admitted.");
        Require(before.SequenceEqual(File.ReadAllBytes(file)), "Reading changed conditions.");
        var shell = new OperatorShellViewModel(new SimulationFoundationService(Path.Combine(directory, "journals")));
        Require(!shell.TryBeginHistoricalReview(), "Uninitialized screen admitted conditions window.");
        await shell.InitializeAsync(CancellationToken.None);
        var previousState = shell.UiState; var starts = shell.TransactionStartCount;
        Require(shell.TryBeginHistoricalReview() && !shell.TryBeginHistoricalReview() && !shell.CanCapture && !shell.CanUseLiveView, "Conditions view did not reserve the existing exclusive gate.");
        try
        {
            var window = new SavedEvaluationConditionsWindow();
            try
            {
                Require(window.ViewModel.LoadFile(actualConditions ?? file), "Conditions producer/consumer compatibility failed.");
                var content = (FrameworkElement)window.Content;
                content.Measure(new Size(860, 600)); content.Arrange(new Rect(0, 0, 860, 600)); content.UpdateLayout();
                var text = string.Join("\n", Descendants<TextBlock>(content).Select(block => block.Text));
                Require(text.Contains("100 DPI", StringComparison.Ordinal) && text.Contains("まだ実行していません", StringComparison.Ordinal), "Bound controls hid the chosen settings or unverified status.");
                var choose = Descendants<Button>(content).Single(button => AutomationProperties.GetAutomationId(button) == "ChooseSavedEvaluationConditions");
                Require(choose.IsEnabled, "Explicit chooser is unavailable.");
                var bitmap = new RenderTargetBitmap(860, 600, 96, 96, PixelFormats.Pbgra32); bitmap.Render(content);
                var artifactRoot = Environment.GetEnvironmentVariable("A0_SAVED_CONDITIONS_TEST_ARTIFACT_DIRECTORY");
                if (!string.IsNullOrEmpty(artifactRoot))
                {
                    Directory.CreateDirectory(artifactRoot);
                    var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
                    using var output = new FileStream(Path.Combine(artifactRoot, "conditions-window-" + Guid.NewGuid().ToString("N") + ".png"), FileMode.CreateNew);
                    encoder.Save(output);
                }
                Require(!window.ViewModel.LoadFile(malformed) && window.ViewModel.Summary.Length == 0, "Window retained stale settings after failure.");
            }
            finally { window.Close(); }
        }
        finally { shell.EndHistoricalReview(); }
        Require(shell.UiState == previousState && shell.TransactionStartCount == starts && !shell.IsBusy, "Viewing settings altered current transaction or result.");
        shell.AcceptSafetyCommand.Execute(null); shell.ToggleLiveViewCommand.Execute(null);
        Require(shell.IsLiveViewActive && !shell.TryBeginHistoricalReview(), "Live view admitted a competing settings window.");
        shell.ToggleLiveViewCommand.Execute(null);
        shell.DualBinding.ReportShutdownBlocked("synthetic-unknown-close");
        Require(!shell.TryBeginHistoricalReview(), "Unknown camera close admitted settings view.");
    }
    private static IEnumerable<T> Descendants<T>(DependencyObject root) where T : DependencyObject
    {
        for (var index = 0; index < VisualTreeHelper.GetChildrenCount(root); ++index)
        {
            var child = VisualTreeHelper.GetChild(root, index);
            if (child is T found) yield return found;
            foreach (var descendant in Descendants<T>(child)) yield return descendant;
        }
    }
    private static void Require(bool condition, string message)
    { if (!condition) throw new Exception(message); }
}
