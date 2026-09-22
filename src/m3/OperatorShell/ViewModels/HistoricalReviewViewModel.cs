using System.Collections.ObjectModel;
using A0CameraStitcher.M3.Foundation;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

internal sealed record HistoricalReviewListItem(OperatorReviewRecord Record)
{
    public string Display => $"{Record.UpdatedAtUtc.LocalDateTime:yyyy-MM-dd HH:mm:ss}  {Record.TransactionId}";
}

internal sealed class HistoricalReviewViewModel : ObservableObject
{
    private HistoricalReviewListItem? _selected;
    private string _status = "履歴を読み込みます…";
    private bool _isBusy;
    private int _offset;
    private int? _nextOffset;
    private readonly Stack<int> _previousOffsets = new();

    public ObservableCollection<HistoricalReviewListItem> Items { get; } = [];
    public HistoricalReviewListItem? Selected
    {
        get => _selected;
        set { if (SetProperty(ref _selected, value)) OnPropertyChanged(nameof(CanOpen)); }
    }
    public string Status { get => _status; set => SetProperty(ref _status, value); }
    public bool IsBusy
    {
        get => _isBusy;
        set
        {
            if (SetProperty(ref _isBusy, value))
            {
                OnPropertyChanged(nameof(CanOpen));
                NotifyNavigation();
            }
        }
    }
    public bool CanPrevious => !IsBusy && _previousOffsets.Count != 0;
    public bool CanNext => !IsBusy && _nextOffset.HasValue;
    public bool CanOpen => !IsBusy && Selected is not null;
    public int Offset => _offset;
    public int? NextOffset => _nextOffset;

    internal void ReplaceItems(IEnumerable<OperatorReviewRecord> records)
    {
        Items.Clear();
        foreach (var record in records) Items.Add(new HistoricalReviewListItem(record));
        Selected = null;
        OnPropertyChanged(nameof(CanOpen));
    }

    internal void ApplyPage(ReviewMetadataPage page, int offset, string expectedReviewKind)
    {
        ArgumentNullException.ThrowIfNull(page);
        if (expectedReviewKind is not ("Product" or "Simulated")) throw new ArgumentException("Review kind is invalid.");
        _offset = offset;
        _nextOffset = page.NextOffset;
        ReplaceItems(page.Records.Where(record => record.State == "Pending" && record.ReviewKind == expectedReviewKind));
        NotifyNavigation();
    }

    internal bool TryGetPreviousOffset(out int offset)
    {
        offset = 0;
        return !IsBusy && _previousOffsets.TryPeek(out offset);
    }

    internal bool TryGetNextOffset(out int offset)
    {
        offset = _nextOffset ?? 0;
        return !IsBusy && _nextOffset.HasValue;
    }

    internal void CommitPreviousNavigation()
    {
        _previousOffsets.Pop();
        NotifyNavigation();
    }

    internal void CommitNextNavigation(int previousOffset)
    {
        _previousOffsets.Push(previousOffset);
        NotifyNavigation();
    }

    private void NotifyNavigation()
    {
        OnPropertyChanged(nameof(CanPrevious));
        OnPropertyChanged(nameof(CanNext));
        OnPropertyChanged(nameof(Offset));
        OnPropertyChanged(nameof(NextOffset));
    }
}
