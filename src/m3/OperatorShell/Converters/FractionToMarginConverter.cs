using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace A0CameraStitcher.M3.OperatorShell.Converters;

/// <summary>
/// Positions a small fixed-size overlay element (the target reticle, or its echo inside the
/// loupe) at a normalized 0..1 fraction of its host container's rendered size, by turning
/// (fractionX, fractionY, containerWidth, containerHeight) into a top-left <see cref="Margin"/>
/// for an element with <c>HorizontalAlignment="Left"</c> and <c>VerticalAlignment="Top"</c>.
/// The optional <see cref="System.Windows.Controls.ContentControl.ConverterParameter"/> (parsed
/// as a double) is the overlay element's own size, so the returned margin centers the overlay
/// on the fractional point instead of anchoring its top-left corner there.
/// </summary>
public sealed class FractionToMarginConverter : IMultiValueConverter
{
    public object Convert(object[] values, Type targetType, object parameter, CultureInfo culture)
    {
        if (values is not [double fractionX, double fractionY, double width, double height] ||
            double.IsNaN(fractionX) || double.IsNaN(fractionY) ||
            double.IsNaN(width) || double.IsNaN(height) ||
            width <= 0 || height <= 0)
        {
            return new Thickness(0);
        }

        var overlaySize = parameter is string parameterText && double.TryParse(parameterText, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsedSize)
            ? parsedSize
            : 0.0;

        var left = (fractionX * width) - (overlaySize / 2.0);
        var top = (fractionY * height) - (overlaySize / 2.0);
        return new Thickness(left, top, 0, 0);
    }

    public object[] ConvertBack(object value, Type[] targetTypes, object parameter, CultureInfo culture) =>
        throw new NotSupportedException("The target reticle position is one-way (view model -> view) only.");
}
