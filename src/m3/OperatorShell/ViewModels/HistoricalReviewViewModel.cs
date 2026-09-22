using System.Collections.ObjectModel;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;

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
    private readonly HashSet<int> _viewedImages = [];
    private HistoricalReviewArtifacts? _reviewedArtifacts;
    private bool _acceptanceAttempted;

    public ObservableCollection<HistoricalReviewListItem> Items { get; } = [];
    public HistoricalReviewListItem? Selected
    {
        get => _selected;
        set
        {
            if (SetProperty(ref _selected, value))
            {
                ResetVerification();
                OnPropertyChanged(nameof(CanOpen));
            }
        }
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
                NotifyAcceptance();
                OnPropertyChanged(nameof(CanSelect));
                NotifyNavigation();
            }
        }
    }
    public bool CanPrevious => !IsBusy && _previousOffsets.Count != 0;
    public bool CanNext => !IsBusy && _nextOffset.HasValue;
    public bool CanOpen => !IsBusy && Selected is not null;
    public bool CanSelect => !IsBusy;
    public int Offset => _offset;
    public int? NextOffset => _nextOffset;
    public bool CanAccept => !IsBusy && !_acceptanceAttempted && _viewedImages.Count > 0 &&
        _reviewedArtifacts is not null && Selected is not null;
    public string VerificationProgress => $"詳細表示済み {_viewedImages.Count}/3（合成・CAM-A・CAM-B）。確認状況は案内です。必要な詳細を確認して「採用を記録」を押してください。";

    internal void RecordViewed(OperatorReviewRecord record, HistoricalReviewArtifacts artifacts, int choice)
    {
        if (choice is < 0 or > 2 || Selected?.Record != record || record.State != "Pending" ||
            artifacts.JobId.ToString("N") != record.ResultId || artifacts.TransactionId.ToString("N") != record.TransactionId ||
            artifacts.ReviewKind != record.ReviewKind) return;
        if (_reviewedArtifacts is not null && (_reviewedArtifacts.ManifestSha256 != artifacts.ManifestSha256 ||
            _reviewedArtifacts.JobId != artifacts.JobId || _reviewedArtifacts.TransactionId != artifacts.TransactionId))
            ResetVerification();
        _reviewedArtifacts = artifacts;
        _viewedImages.Add(choice);
        NotifyAcceptance();
    }

    internal bool TryBeginAcceptance(out OperatorReviewRecord? record, out HistoricalReviewArtifacts? artifacts)
    {
        record = null;
        artifacts = null;
        if (!CanAccept) return false;
        record = Selected!.Record;
        artifacts = _reviewedArtifacts;
        _acceptanceAttempted = true;
        IsBusy = true;
        NotifyAcceptance();
        return true;
    }

    internal void RecordAcceptance(OperatorReviewRecord accepted)
    {
        var item = Items.FirstOrDefault(item => item.Record.ResultId == accepted.ResultId &&
            item.Record.TransactionId == accepted.TransactionId && item.Record.ReviewKind == accepted.ReviewKind);
        if (accepted.State != "Accepted" || item is null) throw new InvalidOperationException("Accepted review target is invalid.");
        Items.Remove(item);
        Selected = null;
    }

    internal void ResetVerification()
    {
        _viewedImages.Clear();
        _reviewedArtifacts = null;
        _acceptanceAttempted = false;
        NotifyAcceptance();
    }

    private void NotifyAcceptance()
    {
        OnPropertyChanged(nameof(CanAccept));
        OnPropertyChanged(nameof(VerificationProgress));
    }

    internal void ReplaceItems(IEnumerable<OperatorReviewRecord> records)
    {
        ResetVerification();
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
