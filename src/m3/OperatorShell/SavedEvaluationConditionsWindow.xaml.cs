using System.Windows;
using Microsoft.Win32;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

namespace A0CameraStitcher.M3.OperatorShell;

public partial class SavedEvaluationConditionsWindow : Window
{
    internal SavedEvaluationConditionsViewModel ViewModel { get; } = new();
    public SavedEvaluationConditionsWindow()
    {
        InitializeComponent();
        DataContext = ViewModel;
    }
    private void OnChooseConditions(object sender, RoutedEventArgs eventArgs)
    {
        var dialog = new OpenFileDialog { Title = "保存した補正条件を選択", Filter = "補正条件 (*.json)|*.json",
            CheckFileExists = true, Multiselect = false };
        if (dialog.ShowDialog(this) == true) ViewModel.LoadFile(dialog.FileName);
    }
    private void OnClose(object sender, RoutedEventArgs eventArgs) => Close();
}
