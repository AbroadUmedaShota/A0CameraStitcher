using System.IO;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

internal sealed class SavedEvaluationConditionsViewModel : ObservableObject
{
    private string _selectedFile = "未選択";
    private string _summary = "";
    private string _status = "保存した補正条件ファイルを選択してください。";
    public string SelectedFile { get => _selectedFile; private set => SetProperty(ref _selectedFile, value); }
    public string Summary { get => _summary; private set => SetProperty(ref _summary, value); }
    public string Status { get => _status; private set => SetProperty(ref _status, value); }

    internal bool LoadFile(string path)
    {
        Summary = ""; SelectedFile = "未選択";
        try
        {
            var selected = SavedEvaluationConditionsSummary.Read(path);
            SelectedFile = Path.GetFileName(path);
            Summary = $"校正条件：{selected.ProfileName}\n{selected.Description}";
            Status = "保存された設定内容を表示しています。";
            return true;
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or NotSupportedException or System.Security.SecurityException)
        {
            Status = "補正条件を読み込めませんでした。対応形式とファイルの内容を確認してください。";
            return false;
        }
    }
}
