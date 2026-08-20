using System.Globalization;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;

namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// Renders SIMULATED live view frames entirely in code (no bitmap assets, no real camera or
/// document images): a synthetic document sample on a dark stage background, optionally
/// rotated (tilt patterns) or blurred (focus-transition pattern), always finished with an
/// unblurred "SIMULATED" watermark and a per-frame alias/timestamp badge so the frame can
/// never be mistaken for a real image.
/// </summary>
public sealed class SimulatedTestImageFrameSource : ISimulatedLiveViewFrameSource
{
    private const int CanvasWidth = 640;
    private const int CanvasHeight = 400;
    private const int BlurCycleLength = 30;
    private const double MaxBlurRadius = 16.0;

    /// <summary>Fraction of <see cref="BlurCycleLength"/> spent ramping from
    /// <see cref="MaxBlurRadius"/> down to sharp focus; the remainder is the sharp hold.</summary>
    private const double BlurRampFraction = 0.6;

    private static readonly Color BackgroundColor = Color.FromRgb(0x14, 0x1A, 0x1F);
    private static readonly Color DocumentColor = Color.FromRgb(0xF3, 0xF0, 0xE6);
    private static readonly Color DocumentBorderColor = Color.FromRgb(0x33, 0x3D, 0x47);
    private static readonly Color ContentLineColor = Color.FromRgb(0xB9, 0xC2, 0xCB);
    private static readonly Color ContentBlockColor = Color.FromRgb(0x8A, 0x96, 0xA3);

    public SimulatedLiveViewFrame CreateFrame(
        string cameraAlias,
        SimulatedFramePattern pattern,
        int sequenceNumber,
        DateTimeOffset capturedAtUtc)
    {
        if (string.IsNullOrWhiteSpace(cameraAlias))
        {
            throw new ArgumentException("A camera alias is required.", nameof(cameraAlias));
        }

        var sceneVisual = new DrawingVisual();
        using (var context = sceneVisual.RenderOpen())
        {
            DrawScene(context, cameraAlias, pattern);
        }

        var blurRadius = pattern == SimulatedFramePattern.BlurToFocusTransition
            ? ComputeBlurRadius(sequenceNumber)
            : 0.0;
        if (pattern == SimulatedFramePattern.BlurToFocusTransition)
        {
            sceneVisual.Effect = new BlurEffect
            {
                Radius = blurRadius,
                KernelType = KernelType.Gaussian,
            };
        }

        // Older WPF (.NET Framework) has a documented quirk where RenderTargetBitmap.Render(visual)
        // ignores an Effect set directly on the visual passed as the root, because Effect is
        // applied by a PARENT compositing a child and a root has no such parent. Verified via
        // SimulatedTestImageFrameSourceAppliesBlurAcrossTheFocusTransition (pixel-diffs seq=0
        // vs seq=17) that on this net10.0-windows WPF build the blur applies correctly either
        // way — wrapping in a child-of-container made no measurable difference (diff ~4.81M
        // BGRA units both with and without it). Kept anyway: it is the documented-safe pattern
        // for that quirk on other WPF versions, costs nothing here, and the pixel-diff test
        // guards against a regression regardless of which form is used.
        var sceneContainer = new ContainerVisual();
        sceneContainer.Children.Add(sceneVisual);
        var sceneBitmap = new RenderTargetBitmap(CanvasWidth, CanvasHeight, 96, 96, PixelFormats.Pbgra32);
        sceneBitmap.Render(sceneContainer);

        // Second pass: composite the (possibly blurred) scene with a crisp, unblurred
        // watermark and timestamp badge, so "Simulated" stays legible at every blur level.
        var overlayVisual = new DrawingVisual();
        using (var context = overlayVisual.RenderOpen())
        {
            context.DrawImage(sceneBitmap, new Rect(0, 0, CanvasWidth, CanvasHeight));
            DrawWatermark(context, cameraAlias, capturedAtUtc);
        }

        var finalBitmap = new RenderTargetBitmap(CanvasWidth, CanvasHeight, 96, 96, PixelFormats.Pbgra32);
        finalBitmap.Render(overlayVisual);
        finalBitmap.Freeze();

        return new SimulatedLiveViewFrame
        {
            CameraAlias = cameraAlias,
            Pattern = pattern,
            SequenceNumber = sequenceNumber,
            CapturedAtUtc = capturedAtUtc,
            Image = finalBitmap,
            Simulation = true,
            Marker = "Simulated",
            BlurRadius = blurRadius,
        };
    }

    private static void DrawScene(DrawingContext context, string cameraAlias, SimulatedFramePattern pattern)
    {
        context.DrawRectangle(new SolidColorBrush(BackgroundColor), null, new Rect(0, 0, CanvasWidth, CanvasHeight));

        var rollDegrees = pattern switch
        {
            SimulatedFramePattern.TiltedDocumentRollMinus6 => -6.0,
            SimulatedFramePattern.TiltedDocumentRollMinus3 => -3.0,
            SimulatedFramePattern.TiltedDocumentRollPlus3 => 3.0,
            SimulatedFramePattern.TiltedDocumentRollPlus6 => 6.0,
            _ => 0.0,
        };

        var documentRect = ComputeDocumentRect();
        var center = new Point(CanvasWidth / 2.0, CanvasHeight / 2.0);
        context.PushTransform(new RotateTransform(rollDegrees, center.X, center.Y));
        context.DrawRectangle(
            new SolidColorBrush(DocumentColor),
            new Pen(new SolidColorBrush(DocumentBorderColor), 2),
            documentRect);
        DrawDocumentContent(context, documentRect);
        context.Pop();

        DrawCameraLabel(context, cameraAlias);
    }

    private static Rect ComputeDocumentRect()
    {
        const double a0PortraitAspect = 841.0 / 1189.0; // ISO A0 width/height ratio.
        var height = CanvasHeight * 0.74;
        var width = height * a0PortraitAspect;
        var x = (CanvasWidth - width) / 2.0;
        var y = (CanvasHeight - height) / 2.0;
        return new Rect(x, y, width, height);
    }

    private static void DrawDocumentContent(DrawingContext context, Rect documentRect)
    {
        var linePen = new Pen(new SolidColorBrush(ContentLineColor), 2);
        const int lineCount = 6;
        var margin = documentRect.Width * 0.08;
        var top = documentRect.Top + (documentRect.Height * 0.14);
        var spacing = documentRect.Height * 0.55 / lineCount;
        for (var index = 0; index < lineCount; index++)
        {
            var y = top + (spacing * index);
            context.DrawLine(linePen, new Point(documentRect.Left + margin, y), new Point(documentRect.Right - margin, y));
        }

        var contentBlockHeight = documentRect.Height * 0.18;
        var contentBlockRect = new Rect(
            documentRect.Left + margin,
            documentRect.Bottom - contentBlockHeight - (documentRect.Height * 0.06),
            documentRect.Width - (margin * 2),
            contentBlockHeight);
        context.DrawRectangle(null, new Pen(new SolidColorBrush(ContentBlockColor), 1.5), contentBlockRect);
    }

    private static void DrawCameraLabel(DrawingContext context, string cameraAlias)
    {
        var typeface = new Typeface(new FontFamily("Segoe UI"), FontStyles.Normal, FontWeights.Bold, FontStretches.Normal);
        var label = CreateFormattedText(cameraAlias, typeface, 18, Brushes.White);
        context.DrawText(label, new Point(16, 12));
    }

    private static void DrawWatermark(DrawingContext context, string cameraAlias, DateTimeOffset capturedAtUtc)
    {
        var watermarkTypeface = new Typeface(new FontFamily("Segoe UI"), FontStyles.Normal, FontWeights.Bold, FontStretches.Normal);
        var watermarkBrush = new SolidColorBrush(Color.FromArgb(120, 255, 255, 255));
        var watermarkText = CreateFormattedText("SIMULATED", watermarkTypeface, 56, watermarkBrush);
        var center = new Point(CanvasWidth / 2.0, CanvasHeight / 2.0);
        context.PushTransform(new RotateTransform(-28, center.X, center.Y));
        context.DrawText(
            watermarkText,
            new Point(center.X - (watermarkText.Width / 2.0), center.Y - (watermarkText.Height / 2.0)));
        context.Pop();

        var badgeTypeface = new Typeface(new FontFamily("Consolas"), FontStyles.Normal, FontWeights.Bold, FontStretches.Normal);
        var badgeText = CreateFormattedText(
            $"{cameraAlias} / Simulated / {capturedAtUtc.ToLocalTime():HH:mm:ss.fff}",
            badgeTypeface,
            13,
            Brushes.White);
        var badgeRect = new Rect(8, CanvasHeight - badgeText.Height - 10, badgeText.Width + 12, badgeText.Height + 6);
        context.DrawRectangle(new SolidColorBrush(Color.FromArgb(160, 0, 0, 0)), null, badgeRect);
        context.DrawText(badgeText, new Point(badgeRect.X + 6, badgeRect.Y + 3));
    }

    private static FormattedText CreateFormattedText(string text, Typeface typeface, double fontSize, Brush brush) =>
        new(
            text,
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            typeface,
            fontSize,
            brush,
            1.0);

    /// <summary>
    /// Ramps from <see cref="MaxBlurRadius"/> down to 0 over the first part of the cycle
    /// (simulating an AF rack into focus), then holds sharp focus for the remainder before
    /// the cycle repeats — giving #31's focus-peaking verification a repeating defocus event.
    /// </summary>
    private static double ComputeBlurRadius(int sequenceNumber)
    {
        var position = ((sequenceNumber % BlurCycleLength) + BlurCycleLength) % BlurCycleLength;
        var rampLength = (int)(BlurCycleLength * BlurRampFraction);
        if (position >= rampLength)
        {
            return 0.0;
        }

        var progress = rampLength <= 1 ? 1.0 : position / (double)(rampLength - 1);
        return MaxBlurRadius * (1.0 - progress);
    }
}
