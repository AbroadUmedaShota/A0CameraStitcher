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

    /// <summary>Column spacing used to pair up top-edge samples for a coarse, robust reference
    /// slope (see phase 1 in <see cref="DetectRollDegrees"/>). That coarse estimate is used only
    /// to tell top-edge columns apart from the much steeper corner/side-edge columns before the
    /// subpixel-resolving least-squares fit in phase 2 — it is not the value this method
    /// returns. Because each paired sample's dy is a difference of integer pixel rows, the
    /// coarse estimate is itself quantized to multiples of 1/SlopeSampleSpacing (0° / 2.8624° /
    /// 5.7106° / … for spacing=20 — see GitHub Issue #84). Widening this constant only makes
    /// that grid finer; it cannot remove the quantization, which is why a previous version of
    /// this detector — that returned the coarse estimate directly — could read a true 1° tilt
    /// as exactly 0°. The value actually returned instead comes from an ordinary least-squares
    /// fit over every top-edge column (phase 2), whose resolution is bounded by integer-pixel
    /// rounding noise divided by the edge's full column span rather than by this spacing.
    /// </summary>
    private const int SlopeSampleSpacing = 20;

    /// <summary>Minimum number of spaced-pair slope samples required before a fit is trusted.
    /// Below this the frame is treated as 検出不能 (no confident detection) rather than
    /// reporting a possibly noise-driven angle — e.g. a frame with no matching document fill
    /// visible at all.</summary>
    private const int MinimumSlopeSampleCount = 30;

    /// <summary>Maximum allowed deviation, in pixels, between a column's detected top-edge row
    /// and the phase-1 coarse reference line before that column is excluded from the phase-2
    /// least-squares fit as a corner/side-edge outlier. Derivation: the coarse slope from phase
    /// 1 is within 1/SlopeSampleSpacing = 0.05 of the true top-edge slope (every spaced-pair
    /// sample is within that bound of the true slope, and so is the median of a set where such
    /// samples are the majority), so a genuine top-edge column up to ~210 columns from the
    /// phase-2 fit's anchor column deviates from the coarse line by at most
    /// 0.05*210 + 1 ≈ 11.5px (the "+1" covers ±0.5px integer-rounding noise at both the anchor
    /// and the column itself). 20px keeps clear margin above that bound while staying well below
    /// a genuine side-edge column's deviation, which is already several pixels just one column
    /// past the corner and grows by roughly cot(θ) per further column (e.g. ~9.5px for the next
    /// column at this detector's largest tested angle, 6°, and much more at smaller angles). At
    /// unusually large angles a single near-corner column can still slip past this filter; its
    /// effect on the fitted angle is small and bounded (see the Issue #84 PR description for the
    /// full argument).</summary>
    private const double InlierResidualToleranceInPixels = 20.0;

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

            // Phase 1: coarse, robust reference slope — the median of dy/dx over column pairs
            // spaced SlopeSampleSpacing apart. A rotated rectangle's upper boundary is two line
            // segments meeting at whichever corner is topmost; for this portrait document shape
            // and the small angles this detector supports, the short (top) edge spans far more
            // columns than the steep (side) edge near that corner, so the median lands on the
            // top edge's slope regardless of rotation sign. This estimate is quantized (see
            // SlopeSampleSpacing's doc comment) and is used only to classify columns in phase 2,
            // never returned directly (GitHub Issue #84).
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
            var coarseSlope = slopes.Count % 2 == 1
                ? slopes[mid]
                : (slopes[mid - 1] + slopes[mid]) / 2.0;

            // Phase 2: subpixel-resolving fit. Collect every column where the document fill was
            // found (not just ones SlopeSampleSpacing apart) and fit an ordinary least-squares
            // line through the (x, topEdgeByColumn[x]) points — unlike phase 1's paired-median
            // approach, an OLS fit's result is not restricted to a 1/SlopeSampleSpacing grid, so
            // it can resolve angles between that grid's steps instead of snapping to them.
            //
            // The top-edge/side-edge split from phase 1 still applies here, so side-edge columns
            // must be excluded before fitting or they would pull the line toward their much
            // steeper slope. The anchor column used for that classification is the MIDDLE valid
            // column by x, not the first or last one: depending on rotation sign, either the
            // leftmost or the rightmost valid columns belong to the steep corner/side edge
            // instead of the top edge (see SimulatedTestImageFrameSource.DrawScene's
            // RotateTransform: a positive/clockwise roll puts that corner on the left, a
            // negative one puts it on the right), but the middle of the valid range is always
            // inside the top edge for the angles this detector supports.
            var validColumns = new List<int>();
            for (var x = 0; x < width; x++)
            {
                if (topEdgeByColumn[x] >= 0)
                {
                    validColumns.Add(x);
                }
            }

            var anchorX = validColumns[validColumns.Count / 2];
            var anchorY = topEdgeByColumn[anchorX];

            double sumX = 0;
            double sumY = 0;
            double sumXY = 0;
            double sumXX = 0;
            var inlierCount = 0;
            foreach (var x in validColumns)
            {
                var predictedY = anchorY + (coarseSlope * (x - anchorX));
                var y = topEdgeByColumn[x];
                if (Math.Abs(y - predictedY) > InlierResidualToleranceInPixels)
                {
                    continue;
                }

                sumX += x;
                sumY += y;
                sumXY += (double)x * y;
                sumXX += (double)x * x;
                inlierCount++;
            }

            if (inlierCount < 2)
            {
                // Should be unreachable once MinimumSlopeSampleCount has already gated on
                // >= 30 spaced pairs, but stay defensive rather than divide by zero below.
                return Math.Atan(coarseSlope) * 180.0 / Math.PI;
            }

            var meanX = sumX / inlierCount;
            var denominator = sumXX - (meanX * sumX);
            if (Math.Abs(denominator) < 1e-6)
            {
                return Math.Atan(coarseSlope) * 180.0 / Math.PI;
            }

            var fittedSlope = (sumXY - (meanX * sumY)) / denominator;
            return Math.Atan(fittedSlope) * 180.0 / Math.PI;
        }
        catch (Exception exception) when (exception is not OutOfMemoryException)
        {
            // Same "never throw" posture as FocusPeakingOverlayRenderer.BuildOverlay: a
            // malformed/unsupported source must fall back to 検出不能, not crash the LV preview.
            return null;
        }
    }
}
