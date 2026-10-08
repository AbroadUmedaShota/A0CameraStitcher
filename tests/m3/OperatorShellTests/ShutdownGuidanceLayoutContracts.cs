using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using A0CameraStitcher.M3.OperatorShell;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

// Issue #236: the shutdown-blocked guidance (four paragraphs plus the technical line) is shown in a
// 760-wide card on the 1920x1080 canvas. This renders the real MainWindow with that card showing and
// measures it, for each wording and for the longest technical line the sanitizer allows, so a card
// that outgrows the canvas or a guidance text that is cut off is caught without a person looking at
// it. It measures layout only; how a screen reader reads the regions is a separate check.
internal static class ShutdownGuidanceLayoutContracts
{
    // The card must leave at least this much canvas above and below it.
    private const double MinimumCanvasMargin = 40;

    internal static async Task RunAsync()
    {
        var root = Path.Combine(Path.GetTempPath(), "A0-ShutdownLayout-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        var window = new MainWindow(root)
        {
            Left = -10000,
            Top = -10000,
            ShowActivated = false,
            ShowInTaskbar = false,
        };
        try
        {
            window.Show();
            var viewModel = (OperatorShellViewModel)window.DataContext;
            var canvas = (Grid)window.FindName("RootCanvas");
            viewModel.DualBinding.IsRequired = true;

            var now = new DateTimeOffset(2026, 10, 6, 3, 0, 0, TimeSpan.Zero);
            var wideCode = new string('網', 160);
            var wideDetail = new string('終', 400);
            var scenarios = new (string Name, Action Report)[]
            {
                ("waiting, known remaining", () => viewModel.DualBinding.ReportShutdownBlocked(
                    "HardwareCameraAgentLaunchException", "Activated capture host did not exit within the bounded wait.",
                    TimeSpan.FromMinutes(8), now)),
                ("waiting, unknown remaining, first report", () => viewModel.DualBinding.ReportShutdownBlocked(
                    "HardwareCameraAgentLaunchException", "detail", null, now)),
                ("waiting, unknown remaining, repeated past the held time", () =>
                {
                    viewModel.DualBinding.ReportShutdownBlocked("HardwareCameraAgentLaunchException", "detail", null, now);
                    viewModel.DualBinding.ReportShutdownBlocked(
                        "HardwareCameraAgentLaunchException", "detail", null, now + TimeSpan.FromMinutes(11));
                }),
                ("budget exhausted", () => viewModel.DualBinding.ReportShutdownBlocked(
                    "HardwareCameraAgentLaunchException", "detail", TimeSpan.Zero, now)),
                ("longest technical line, wide characters", () => viewModel.DualBinding.ReportShutdownBlocked(
                    wideCode, wideDetail, null, now)),
            };

            foreach (var (name, report) in scenarios)
            {
                report();
                window.UpdateLayout();
                var text = viewModel.DualBinding.InvalidationText;
                var card = FindDescendants<Border>(canvas).SingleOrDefault(border =>
                    border.Width == 760 && FindDescendants<TextBlock>(border).Any(block => block.Text == text))
                    ?? throw new InvalidOperationException($"{name}: the 760-wide binding card with the guidance was not found.");
                var guidance = FindDescendants<TextBlock>(card).Single(block => block.Text == text);

                Require(card.IsVisible && guidance.IsVisible, $"{name}: the card and the guidance must be on screen.");
                Require(card.ActualWidth == 760, $"{name}: the card must keep its 760 width; was {card.ActualWidth}.");
                Require(guidance.TextWrapping == TextWrapping.Wrap, $"{name}: the guidance must wrap.");
                Require(guidance.ActualWidth <= card.ActualWidth,
                    $"{name}: the guidance ({guidance.ActualWidth}) must not be wider than the card ({card.ActualWidth}).");
                Require(
                    card.ActualHeight + 2 * MinimumCanvasMargin <= canvas.ActualHeight,
                    $"{name}: the card is {card.ActualHeight} tall on a {canvas.ActualHeight} canvas; " +
                    $"it must leave {MinimumCanvasMargin} above and below.");
                // The guidance block's own height grows with its lines; its measured desired height
                // must fit inside what it was given, so no line is clipped.
                Require(
                    guidance.DesiredSize.Height <= guidance.ActualHeight + 0.5,
                    $"{name}: the guidance wants {guidance.DesiredSize.Height} but was given {guidance.ActualHeight}.");
                Console.WriteLine(string.Create(CultureInfo.InvariantCulture,
                    $"INFO #236 layout [{name}]: card {card.ActualWidth:0}x{card.ActualHeight:0} on a {canvas.ActualWidth:0}x{canvas.ActualHeight:0} canvas, guidance {guidance.ActualHeight:0} tall"));
            }

            MeasureVerticalTab();
        }
        finally
        {
            window.Close();
            await Task.Yield();
            Directory.Delete(root, recursive: true);
        }
    }

    // Issue #236 asked whether WPF breaks a line at U+000B. Measured here for the record; the sanitizer
    // collapses it either way, so this asserts nothing about the answer.
    private static void MeasureVerticalTab()
    {
        double Height(string value)
        {
            var block = new TextBlock { Text = value, TextWrapping = TextWrapping.Wrap, Width = 2000 };
            block.Measure(new Size(2000, double.PositiveInfinity));
            return block.DesiredSize.Height;
        }

        var single = Height("first second");
        var withVerticalTab = Height("first\vsecond");
        var withLineFeed = Height("first\nsecond");
        Console.WriteLine(string.Create(CultureInfo.InvariantCulture,
            $"INFO #236 WPF TextBlock height: one line {single:0.0}, with U+000B {withVerticalTab:0.0}, with LF {withLineFeed:0.0}; " +
            $"U+000B breaks the line: {withVerticalTab > single + 1}"));
        Require(
            HardwareDualWindowShutdownOutcome.SanitizeForOperatorDisplay("first\vsecond") == "first second",
            "A vertical tab must be collapsed to a space whatever WPF does with it.");
    }

    private static IEnumerable<T> FindDescendants<T>(DependencyObject parent) where T : DependencyObject
    {
        for (var index = 0; index < VisualTreeHelper.GetChildrenCount(parent); index++)
        {
            var child = VisualTreeHelper.GetChild(parent, index);
            if (child is T match)
            {
                yield return match;
            }

            foreach (var descendant in FindDescendants<T>(child))
            {
                yield return descendant;
            }
        }
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
