using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// Builds a preview-only "focus peaking" overlay (issue #31) for a SIMULATED live view frame:
/// a mostly transparent bitmap, the same size as the source, where pixels sitting on a strong
/// local luminance gradient (an edge) are painted a bright highlight color and every other
/// pixel is fully transparent alpha=0. Callers composite the returned bitmap on top of the
/// existing LV <c>Image</c> element in XAML; this never modifies or replaces the source frame
/// itself, so it stays clear of the "preview never becomes original/stitch input" contract
/// documented in docs/OPERATOR_UI_SPEC.md's フォーカス操作 section — there is no code path from
/// this overlay back into original-image or stitch-input handling.
/// </summary>
public static class FocusPeakingOverlayRenderer
{
    /// <summary>Minimum luminance delta (0..255 scale) between a pixel and its right/below
    /// neighbor to be painted as an edge. Chosen empirically against the synthetic document
    /// scene (line art on a light background over a dark stage) so the drawn content lines and
    /// document border highlight clearly without flagging the flat background or flat document
    /// fill as "edges".</summary>
    private const int EdgeLuminanceThreshold = 40;

    private static readonly Color HighlightColor = Color.FromRgb(0xFF, 0x2E, 0x2E);

    /// <summary>Returns the highlight overlay for <paramref name="source"/>, or null when there
    /// is nothing meaningful to highlight (no source, or a degenerate 1x1 frame such as the
    /// fake frame source used by headless tests) or when the source could not be read.</summary>
    public static BitmapSource? BuildOverlay(BitmapSource? source)
    {
        if (source is null || source.PixelWidth < 2 || source.PixelHeight < 2)
        {
            return null;
        }

        try
        {
            var converted = new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0);
            var width = converted.PixelWidth;
            var height = converted.PixelHeight;
            var stride = width * 4;
            var pixels = new byte[stride * height];
            converted.CopyPixels(pixels, stride, 0);

            var overlay = new byte[stride * height];
            for (var y = 0; y < height; y++)
            {
                for (var x = 0; x < width; x++)
                {
                    var offset = (y * stride) + (x * 4);
                    var luminance = Luminance(pixels, offset);

                    var horizontalDelta = x + 1 < width
                        ? Math.Abs(luminance - Luminance(pixels, offset + 4))
                        : 0;
                    var verticalDelta = y + 1 < height
                        ? Math.Abs(luminance - Luminance(pixels, offset + stride))
                        : 0;

                    if (Math.Max(horizontalDelta, verticalDelta) < EdgeLuminanceThreshold)
                    {
                        continue;
                    }

                    overlay[offset] = HighlightColor.B;
                    overlay[offset + 1] = HighlightColor.G;
                    overlay[offset + 2] = HighlightColor.R;
                    overlay[offset + 3] = 0xFF;
                }
            }

            var dpiX = source.DpiX > 0 ? source.DpiX : 96;
            var dpiY = source.DpiY > 0 ? source.DpiY : 96;
            var bitmap = BitmapSource.Create(width, height, dpiX, dpiY, PixelFormats.Bgra32, null, overlay, stride);
            bitmap.Freeze();
            return bitmap;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            // The overlay is a soft cosmetic aid, not a required contract: a malformed or
            // unsupported source bitmap must never crash the LV preview (same "never throw"
            // posture as OperatorShellViewModel.ApplySimulatedFrameTick).
            return null;
        }
    }

    private static int Luminance(byte[] pixels, int bgraOffset)
    {
        var blue = pixels[bgraOffset];
        var green = pixels[bgraOffset + 1];
        var red = pixels[bgraOffset + 2];
        return ((red * 299) + (green * 587) + (blue * 114)) / 1000;
    }
}
