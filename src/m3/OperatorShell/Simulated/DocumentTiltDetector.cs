using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// Preview-only in-plane rotation (ROLL) estimator for issue #32's 傾き読み値: a pure function
/// over a rendered LV frame's pixels that never touches original-image or stitch-input state
/// (same "preview stays preview" posture as <see cref="FocusPeakingOverlayRenderer"/> — there is
/// no code path from this detector back into original-image, capture, or readiness handling).
///
/// It looks for the synthetic document's warm cream fill
/// (<see cref="SimulatedTestImageFrameSource"/>'s <c>DocumentColor</c>, approx RGB(243,240,230))
/// against everything else a rendered SIMULATED frame can contain: the dark stage background,
/// the neutral-white camera-alias label, the semi-transparent "SIMULATED" watermark composited
/// over the background, and the document's own cooler-toned border/content-line colors — using a
/// combined luminance + red-minus-blue "warmth" test tuned empirically against that specific
/// synthetic scene (see the constants below, and
/// <c>SimulatedTestImageFrameSourceDocumentColorsSeparateFromWatermarkAndLabel</c> in the test
/// suite for the measured separation). This is not a general-purpose document scanner and makes
/// no accuracy claim outside that scene.
/// </summary>
public static class DocumentTiltDetector
{
    /// <summary>Minimum 0..255 luminance to consider a pixel part of the document fill. The
    /// document interior renders at ~240; the "SIMULATED" watermark composited over the dark
    /// background renders at ~133, and the document's own border/content-line colors render at
    /// ~60-192 — both comfortably below this.</summary>
    private const int DocumentLuminanceThreshold = 200;

    /// <summary>Minimum (red - blue) to consider a pixel part of the document's warm cream
    /// fill, rather than the neutral-white camera-alias label (red-blue ≈ 0) or the document's
    /// own cooler-toned content-line/border colors (red-blue &lt; 0). The document fill itself
    /// sits at red-blue ≈ 13.</summary>
    private const int DocumentWarmthThreshold = 5;

    /// <summary>Column spacing used to pair up top-edge samples for a slope estimate (see
    /// <see cref="DetectRollDegrees"/>). The angles this detector targets (a few degrees) move
    /// the boundary well under one pixel per column, so a 1-column spacing quantizes almost
    /// every pair to dy=0 and a median of those collapses to exactly 0 regardless of the true
    /// angle. Spacing samples this far apart keeps each pair's dy large enough (several pixels
    /// at these angles) for integer pixel rounding to still resolve a fraction-of-a-degree
    /// difference.</summary>
    private const int SlopeSampleSpacing = 20;

    /// <summary>Minimum number of spaced-pair slope samples required before a fit is trusted.
    /// Below this the frame is treated as 検出不能 (no confident detection) rather than
    /// reporting a possibly noise-driven angle — e.g. a frame with no matching document fill
    /// visible at all.</summary>
    private const int MinimumSlopeSampleCount = 30;

    /// <summary>
    /// Estimates the document's in-plane rotation (ROLL) in degrees from <paramref name="source"/>,
    /// or returns null when detection is not possible (no/degenerate source, or too few columns
    /// where the document fill color could be found) — the caller's cue to show 検出不能 instead
    /// of a number. Positive degrees match the same clockwise-positive convention
    /// <see cref="System.Windows.Media.RotateTransform"/> uses to draw the SIMULATED tilt test
    /// patterns, so a <c>TiltedDocumentRollPlus6</c> frame is expected to report approximately
    /// +6.
    /// </summary>
    public static double? DetectRollDegrees(BitmapSource? source)
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

            // For each column, the row of the first pixel (scanning top-down) whose color
            // matches the document's warm cream fill — the top boundary of the (possibly
            // rotated) document rectangle in that column. -1 means the column has no match.
            var topEdgeByColumn = new int[width];
            for (var x = 0; x < width; x++)
            {
                topEdgeByColumn[x] = -1;
                for (var y = 0; y < height; y++)
                {
                    var offset = (y * stride) + (x * 4);
                    var blue = pixels[offset];
                    var green = pixels[offset + 1];
                    var red = pixels[offset + 2];
                    var luminance = ((red * 299) + (green * 587) + (blue * 114)) / 1000;
                    if (luminance >= DocumentLuminanceThreshold && (red - blue) >= DocumentWarmthThreshold)
                    {
                        topEdgeByColumn[x] = y;
                        break;
                    }
                }
            }

            // Robust slope estimate: the median of dy/dx over column pairs spaced
            // SlopeSampleSpacing apart. A rotated rectangle's upper boundary is two line
            // segments meeting at whichever corner is topmost; for this portrait document shape
            // and the small angles this detector supports, the short (top) edge spans far more
            // columns than the steep (side) edge near that corner, so the median lands on the
            // top edge's slope regardless of rotation sign.
            var slopes = new List<double>();
            for (var x = 0; x + SlopeSampleSpacing < width; x++)
            {
                if (topEdgeByColumn[x] >= 0 && topEdgeByColumn[x + SlopeSampleSpacing] >= 0)
                {
                    slopes.Add((topEdgeByColumn[x + SlopeSampleSpacing] - topEdgeByColumn[x]) / (double)SlopeSampleSpacing);
                }
            }

            if (slopes.Count < MinimumSlopeSampleCount)
            {
                return null;
            }

            slopes.Sort();
            var mid = slopes.Count / 2;
            var medianSlope = slopes.Count % 2 == 1
                ? slopes[mid]
                : (slopes[mid - 1] + slopes[mid]) / 2.0;

            return Math.Atan(medianSlope) * 180.0 / Math.PI;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            // Same "never throw" posture as FocusPeakingOverlayRenderer.BuildOverlay: a
            // malformed/unsupported source must fall back to 検出不能, not crash the LV preview.
            return null;
        }
    }
}
