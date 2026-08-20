using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace A0CameraStitcher.M3.OperatorShell.Converters;

/// <summary>
/// Shows an element only when the bound string equals <see cref="System.Windows.Data.Binding.ConverterParameter"/>
/// (ordinal comparison), otherwise collapses it. Used to switch between the operator shell's
/// mutually-exclusive dashboard/maintenance pages (<c>OperatorShellViewModel.SelectedPage</c>,
/// issue #34) without a <c>TabControl</c> — each page's root element occupies the same grid
/// cell and only one is visible at a time, driven purely by view model state.
/// </summary>
public sealed class StringEqualsVisibilityConverter : IValueConverter
{
    public object Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        value is string current && parameter is string expected && string.Equals(current, expected, StringComparison.Ordinal)
            ? Visibility.Visible
            : Visibility.Collapsed;

    public object ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException("The selected operator shell page is one-way (view model -> view) only.");
}
