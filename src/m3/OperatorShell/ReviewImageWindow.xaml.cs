using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace A0CameraStitcher.M3.OperatorShell;

/// <summary>A read-only, full-resolution viewer outside the dashboard's scaled canvas.</summary>
public partial class ReviewImageWindow : Window
{
    private readonly BitmapSource _bitmap;
    private bool _actualPixels;

    public ReviewImageWindow(string imagePath, bool showSeam)
    {
        // OnLoad detaches the view from the original: no file lock or write remains.
        using (var stream = new FileStream(imagePath, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            var decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat,
                BitmapCacheOption.OnLoad);
            _bitmap = decoder.Frames[0];
            _bitmap.Freeze();
        }
        InitializeComponent();
        ReviewImage.Source = _bitmap;
        SeamButton.Visibility = showSeam ? Visibility.Visible : Visibility.Collapsed;
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

    private void OnCenter(object sender, RoutedEventArgs e)
    {
        _actualPixels = true;
        UpdateScale();
        Dispatcher.BeginInvoke(DispatcherPriority.Loaded, new Action(() =>
        {
            ImageScroll.ScrollToHorizontalOffset(ImageScroll.ScrollableWidth / 2);
            ImageScroll.ScrollToVerticalOffset(ImageScroll.ScrollableHeight / 2);
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
