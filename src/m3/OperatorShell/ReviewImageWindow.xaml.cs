using System.IO;
using System.Security.Cryptography;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using A0CameraStitcher.M3.Foundation.DualCamera;

namespace A0CameraStitcher.M3.OperatorShell;

/// <summary>A read-only, full-resolution viewer outside the dashboard's scaled canvas.</summary>
public partial class ReviewImageWindow : Window
{
    private readonly BitmapSource _bitmap;
    private readonly StitchSeamNavigationPoint? _seamPoint;
    private bool _actualPixels;

    internal ReviewImageWindow(string imagePath, StitchSeamNavigationPoint? seamPoint, string? expectedSha256 = null)
    {
        // OnLoad detaches the view from the original: no file lock or write remains.
        using (var stream = new FileStream(imagePath, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            // Historical verification releases its locks before this window opens.
            // Hash and decode the same read-locked stream, not two path lookups.
            if (expectedSha256 is not null)
            {
                var actual = Convert.ToHexString(SHA256.HashData(stream));
                if (!string.Equals(actual, expectedSha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Review image changed after verification.");
                stream.Position = 0;
            }
            var decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat,
                BitmapCacheOption.OnLoad);
            _bitmap = decoder.Frames[0];
            _bitmap.Freeze();
        }
        InitializeComponent();
        ReviewImage.Source = _bitmap;
        _seamPoint = seamPoint is { XPixels: >= 0, YPixels: >= 0 } point &&
            point.OutputWidth == _bitmap.PixelWidth && point.OutputHeight == _bitmap.PixelHeight &&
            point.XPixels < _bitmap.PixelWidth && point.YPixels < _bitmap.PixelHeight
            ? point
            : null;
        SeamButton.IsEnabled = _seamPoint is not null;
        SeamButton.ToolTip = _seamPoint is null
            ? "実つなぎ目の記録がないか、確認できません。画像中央の代用はしません。"
            : "合成時に記録された実つなぎ目位置へ100%表示で移動します。";
        Loaded += (_, _) => UpdateScale();
        DpiChanged += (_, _) => UpdateScale();
    }

    private void OnFit(object sender, RoutedEventArgs e)
    {
        _actualPixels = false;
        UpdateScale();
    }

    private void OnActualPixels(object sender, RoutedEventArgs e)
    {
        _actualPixels = true;
        UpdateScale();
    }

    private void OnSeam(object sender, RoutedEventArgs e)
    {
        if (_seamPoint is null) return;
        _actualPixels = true;
        UpdateScale();
        Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
        {
            var dpi = VisualTreeHelper.GetDpi(this);
            var xInDeviceIndependentPixels = _seamPoint.XPixels / dpi.DpiScaleX;
            var yInDeviceIndependentPixels = _seamPoint.YPixels / dpi.DpiScaleY;
            ImageScroll.ScrollToHorizontalOffset(Math.Clamp(
                xInDeviceIndependentPixels - ImageScroll.ViewportWidth / 2,
                0,
                ImageScroll.ScrollableWidth));
            ImageScroll.ScrollToVerticalOffset(Math.Clamp(
                yInDeviceIndependentPixels - ImageScroll.ViewportHeight / 2,
                0,
                ImageScroll.ScrollableHeight));
        }));
    }

    private void OnViewportChanged(object sender, SizeChangedEventArgs e)
    {
        if (IsLoaded) UpdateScale();
    }

    private void UpdateScale()
    {
        var dpi = VisualTreeHelper.GetDpi(this);
        var nativeWidth = _bitmap.PixelWidth / dpi.DpiScaleX;
        var nativeHeight = _bitmap.PixelHeight / dpi.DpiScaleY;
        var factor = _actualPixels ? 1 : Math.Min(1, Math.Min(
            Math.Max(1, ImageScroll.ActualWidth - 24) / nativeWidth,
            Math.Max(1, ImageScroll.ActualHeight - 24) / nativeHeight));
        ReviewImage.Width = nativeWidth * factor;
        ReviewImage.Height = nativeHeight * factor;
        ZoomLabel.Text = $"{(_actualPixels ? "100%（実ピクセル）" : "全体")}  /  {_bitmap.PixelWidth} × {_bitmap.PixelHeight}";
    }
}
