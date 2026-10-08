using System.Globalization;
using System.IO;
using System.Windows;
using System.Windows.Automation.Peers;
using System.Windows.Controls;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Threading;
using System.Xml.Linq;
using A0CameraStitcher.M3.OperatorShell.Controls;
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
                // A card taller than half the canvas is no longer a notice on top of the screen; it is
                // the screen. The longest wording must stay below that.
                Require(
                    card.ActualHeight <= canvas.ActualHeight / 2,
                    $"{name}: the card is {card.ActualHeight} tall; it must not exceed half of the {canvas.ActualHeight} canvas.");
                // The guidance block's own height grows with its lines; its measured desired height
                // must fit inside what it was given, so no line is clipped.
                Require(
                    guidance.DesiredSize.Height <= guidance.ActualHeight + 0.5,
                    $"{name}: the guidance wants {guidance.DesiredSize.Height} but was given {guidance.ActualHeight}.");
                Console.WriteLine(string.Create(CultureInfo.InvariantCulture,
                    $"INFO #236 layout [{name}]: card {card.ActualWidth:0}x{card.ActualHeight:0} on a {canvas.ActualWidth:0}x{canvas.ActualHeight:0} canvas, guidance {guidance.ActualHeight:0} tall"));
            }

            MeasureVerticalTab();
            await RequireRepeatedCloseAttemptIsAnnouncedAsync(window, viewModel, canvas, now);
        }
        finally
        {
            window.Close();
            await Task.Yield();
            Directory.Delete(root, recursive: true);
        }

        await MeasureSingleCameraOverlayAtMinimumWindowSizeAsync();
    }

    // Pressing close again reports the same texts. WPF raises no change notification for an unchanged
    // property, so unless the report passes through an empty value a screen reader is silent on every
    // attempt after the first. Both regions that carry the report are checked on the real MainWindow:
    // the guidance (assertive) and the short notice under the buttons (polite).
    private static async Task RequireRepeatedCloseAttemptIsAnnouncedAsync(
        MainWindow window,
        OperatorShellViewModel viewModel,
        Grid canvas,
        DateTimeOffset now)
    {
        var originalListenerExists = LiveRegion.ListenerExists;
        var originalRaise = LiveRegion.RaiseLiveRegionChanged;
        var raised = new List<AutomationPeer>();
        LiveRegion.ListenerExists = static () => true;
        LiveRegion.RaiseLiveRegionChanged = peer => raised.Add(peer);
        try
        {
            // Loaded has to have run for the regions to announce at all.
            await window.Dispatcher.InvokeAsync(static () => { }, DispatcherPriority.ApplicationIdle);
            Require(window.IsLoaded, "The window must be loaded before announcements are observed.");

            var binding = viewModel.DualBinding;
            const string code = "HardwareCameraAgentLaunchException";
            binding.ReportShutdownBlocked(code, "detail", TimeSpan.FromMinutes(4), now);
            window.UpdateLayout();
            var guidance = FindAnnouncedRegion(canvas, binding.InvalidationText);
            var notice = FindAnnouncedRegion(canvas, binding.NoticeText);
            Require(raised.Count(peer => OwnerOf(peer, guidance)) >= 1,
                "The first blocked report must announce the guidance.");
            Require(raised.Count(peer => OwnerOf(peer, notice)) >= 1,
                "The first blocked report must announce the notice.");

            for (var attempt = 2; attempt <= 3; attempt++)
            {
                raised.Clear();
                binding.ReportShutdownBlocked(code, "detail", TimeSpan.FromMinutes(4), now);
                window.UpdateLayout();
                Require(raised.Count(peer => OwnerOf(peer, guidance)) == 1,
                    $"Attempt {attempt}: an identical blocked report must announce the guidance again, once; " +
                    $"raised {raised.Count(peer => OwnerOf(peer, guidance))}.");
                Require(raised.Count(peer => OwnerOf(peer, notice)) == 1,
                    $"Attempt {attempt}: an identical blocked report must announce the notice again, once; " +
                    $"raised {raised.Count(peer => OwnerOf(peer, notice))}.");
                Require(binding.InvalidationText.Length > 0 && binding.NoticeText.Length > 0,
                    $"Attempt {attempt}: the texts must end up shown, not left empty by the pass through an empty value.");
            }
        }
        finally
        {
            LiveRegion.ListenerExists = originalListenerExists;
            LiveRegion.RaiseLiveRegionChanged = originalRaise;
        }

        static bool OwnerOf(AutomationPeer peer, TextBlock region) =>
            peer is UIElementAutomationPeer { Owner: var owner } && ReferenceEquals(owner, region);

        static TextBlock FindAnnouncedRegion(DependencyObject root, string text) =>
            FindDescendants<TextBlock>(root).Single(block => block.Text == text && LiveRegion.GetAnnounce(block));
    }

    // Issue #244: the SingleCamera window shows its own close indicator, three lines in a 620-wide
    // card over the whole window. The window's smallest size is 1020x700, so that is where the card
    // is most likely to be cut off. HardwareSingleCameraWindow cannot be constructed here (it takes
    // the exclusive session lease and opens the operator's real storage), so the overlay element is
    // read from the window's own XAML and hosted in a window of the same size, bound to the real
    // texts with all three lines showing. The XAML is located from the build output; this is a layout
    // check only.
    private static async Task MeasureSingleCameraOverlayAtMinimumWindowSizeAsync()
    {
        const string OverlayName = "終了の確認中";
        var xamlPath = FindSourceFile("src/m3/OperatorShell/HardwareSingleCameraWindow.xaml");
        var windowXml = XDocument.Load(xamlPath);
        var overlay = windowXml.Descendants().Single(element =>
            element.Attributes().Any(attribute =>
                attribute.Name.LocalName == "AutomationProperties.Name" && attribute.Value == OverlayName));
        var minWidth = double.Parse(windowXml.Root!.Attribute("MinWidth")!.Value, CultureInfo.InvariantCulture);
        var minHeight = double.Parse(windowXml.Root!.Attribute("MinHeight")!.Value, CultureInfo.InvariantCulture);
        Require(minWidth == 1020 && minHeight == 700,
            $"The window minimum size is expected to be 1020x700; found {minWidth}x{minHeight}. Update this check with it.");

        var assemblyName = typeof(LiveRegion).Assembly.GetName().Name;
        var overlayXaml = overlay.ToString().Replace(
            "clr-namespace:A0CameraStitcher.M3.OperatorShell.Controls",
            "clr-namespace:A0CameraStitcher.M3.OperatorShell.Controls;assembly=" + assemblyName,
            StringComparison.Ordinal);
        var overlayElement = (Grid)XamlReader.Parse(overlayXaml);

        var host = new Grid();
        host.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        host.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        host.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        host.Children.Add(overlayElement);
        var window = new Window
        {
            Content = host,
            Width = minWidth,
            Height = minHeight,
            FontFamily = new FontFamily("Yu Gothic UI"),
            Left = -10000,
            Top = -10000,
            ShowActivated = false,
            ShowInTaskbar = false,
            DataContext = new SingleOverlayTexts(),
        };
        try
        {
            window.Show();
            await window.Dispatcher.InvokeAsync(static () => { }, DispatcherPriority.ApplicationIdle);
            window.UpdateLayout();

            Require(overlayElement.IsVisible, "The close indicator must be showing while IsShutdownConfirming is true.");
            var card = FindDescendants<Border>(overlayElement).Single(border => border.Width == 620);
            var lines = FindDescendants<TextBlock>(card).ToList();
            Require(lines.Count == 3, $"The close indicator is expected to have three lines; found {lines.Count}.");
            foreach (var line in lines)
            {
                Require(line.IsVisible && line.Text.Length > 0, "All three lines must be showing for this check.");
                Require(line.TextWrapping == TextWrapping.Wrap, "Each line must wrap.");
                // DesiredSize includes the line's own margin; ActualHeight does not.
                var wanted = line.DesiredSize.Height - line.Margin.Top - line.Margin.Bottom;
                Require(wanted <= line.ActualHeight + 0.5,
                    $"A line wants {wanted} but was given {line.ActualHeight}; it is cut off.");
            }

            Require(card.ActualWidth == 620, $"The card must keep its 620 width; was {card.ActualWidth}.");
            Require(card.ActualHeight + 2 * MinimumCanvasMargin <= host.ActualHeight,
                $"The card is {card.ActualHeight} tall in a {host.ActualHeight} tall window; " +
                $"it must leave {MinimumCanvasMargin} above and below.");
            Require(card.ActualHeight <= host.ActualHeight / 2,
                $"The card is {card.ActualHeight} tall; it must not exceed half of the {host.ActualHeight} tall window.");
            Require(card.ActualWidth + 2 * MinimumCanvasMargin <= host.ActualWidth,
                $"The card is {card.ActualWidth} wide in a {host.ActualWidth} wide window.");
            Console.WriteLine(string.Create(CultureInfo.InvariantCulture,
                $"INFO #244 layout [single-camera close indicator, 3 lines]: card {card.ActualWidth:0}x{card.ActualHeight:0} in a {host.ActualWidth:0}x{host.ActualHeight:0} window"));
        }
        finally
        {
            window.Close();
            await Task.Yield();
        }
    }

    // The three texts and the flag the overlay binds to, taken from the ViewModel's own constants.
    private sealed class SingleOverlayTexts
    {
        public bool IsShutdownConfirming => true;

        public string ShutdownConfirmingText => HardwareSingleCameraViewModel.ShutdownConfirmingMessage;

        public string ShutdownConfirmingDetailText => HardwareSingleCameraViewModel.ShutdownConfirmingDetailMessage;

        public string ShutdownFrameWaitText => HardwareSingleCameraViewModel.ShutdownFrameWaitMessage;
    }

    private static string FindSourceFile(string relativePath)
    {
        for (var directory = new DirectoryInfo(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
        {
            var candidate = Path.Combine(directory.FullName, relativePath);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        throw new FileNotFoundException($"{relativePath} was not found above {AppContext.BaseDirectory}.");
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
