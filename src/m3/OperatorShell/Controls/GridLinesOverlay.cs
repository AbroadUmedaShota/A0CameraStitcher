using System.Windows;
using System.Windows.Media;

namespace A0CameraStitcher.M3.OperatorShell.Controls;

/// <summary>
/// ステージへ重ねる構図グリッド。表示領域を列数・行数でちょうど等分する。
/// タイル塗り（DrawingBrush）ではステージの実寸に関係なく一定間隔になってしまい、
/// 「3×3」「4×4」といった構図の指定にならないため、線を都度引く。
/// 位置合わせの目安であり、撮影の可否判定には使わない。
/// </summary>
public sealed class GridLinesOverlay : FrameworkElement
{
    public static readonly DependencyProperty ColumnsProperty = DependencyProperty.Register(
        nameof(Columns),
        typeof(int),
        typeof(GridLinesOverlay),
        new FrameworkPropertyMetadata(3, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty RowsProperty = DependencyProperty.Register(
        nameof(Rows),
        typeof(int),
        typeof(GridLinesOverlay),
        new FrameworkPropertyMetadata(3, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty LineBrushProperty = DependencyProperty.Register(
        nameof(LineBrush),
        typeof(Brush),
        typeof(GridLinesOverlay),
        new FrameworkPropertyMetadata(Brushes.Gray, FrameworkPropertyMetadataOptions.AffectsRender));

    public GridLinesOverlay() => IsHitTestVisible = false;

    public int Columns
    {
        get => (int)GetValue(ColumnsProperty);
        set => SetValue(ColumnsProperty, value);
    }

    public int Rows
    {
        get => (int)GetValue(RowsProperty);
        set => SetValue(RowsProperty, value);
    }

    public Brush LineBrush
    {
        get => (Brush)GetValue(LineBrushProperty);
        set => SetValue(LineBrushProperty, value);
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        var width = ActualWidth;
        var height = ActualHeight;
        if (width <= 0 || height <= 0)
        {
            return;
        }

        var brush = LineBrush;
        if (brush is null)
        {
            return;
        }

        // 1px の罫を物理ピクセルの境界へ寄せて、線ごとの濃さのばらつきを避ける。
        var pen = new Pen(brush, 1);
        pen.Freeze();

        for (var column = 1; column < Columns; column++)
        {
            var x = Math.Round(width * column / Columns) + 0.5;
            drawingContext.DrawLine(pen, new Point(x, 0), new Point(x, height));
        }

        for (var row = 1; row < Rows; row++)
        {
            var y = Math.Round(height * row / Rows) + 0.5;
            drawingContext.DrawLine(pen, new Point(0, y), new Point(width, y));
        }
    }
}
