namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// Production pump backed by <see cref="System.Threading.Timer"/> — a thread-pool timer, not
/// a WPF DispatcherTimer — so it carries no UI-thread affinity. It intentionally does none of
/// the WPF rendering itself: each tick is a cheap alias/pattern/sequence/generation record
/// only, which keeps timer callbacks near-instant (no overlapping-tick risk from a slow
/// render) and means no thread-pool thread ever needs its own lazily-created Dispatcher just
/// to service this pump. The subscriber (OperatorShellViewModel) calls
/// <see cref="ISimulatedLiveViewFrameSource.CreateFrame"/> itself, on whichever thread it
/// marshals the tick to — in production, the UI thread.
/// </summary>
public sealed class SimulatedLiveViewFramePump : ISimulatedLiveViewFramePump
{
    private readonly ISimulatedFrameClock _clock;
    private readonly TimeSpan _interval;
    private readonly object _gate = new();
    private Timer? _timer;
    private string? _activeCameraAlias;
    private SimulatedFramePattern _pattern;
    private int _sequenceNumber;
    private int _generation;
    private bool _disposed;

    public SimulatedLiveViewFramePump(ISimulatedFrameClock? clock = null, TimeSpan? interval = null)
    {
        _clock = clock ?? new SystemSimulatedFrameClock();
        _interval = interval ?? TimeSpan.FromMilliseconds(200);
        if (_interval <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(interval), "The frame interval must be positive.");
        }
    }

    public event EventHandler<SimulatedLiveViewFrameTick>? Tick;

    public int Start(string cameraAlias, SimulatedFramePattern pattern)
    {
        if (string.IsNullOrWhiteSpace(cameraAlias))
        {
            throw new ArgumentException("A camera alias is required to start the SIMULATED frame pump.", nameof(cameraAlias));
        }

        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            _activeCameraAlias = cameraAlias;
            _pattern = pattern;
            _sequenceNumber = 0;
            _generation++;
            _timer?.Dispose();
            _timer = new Timer(OnTick, null, TimeSpan.Zero, _interval);
            return _generation;
        }
    }

    public void Stop()
    {
        lock (_gate)
        {
            _timer?.Dispose();
            _timer = null;
            _activeCameraAlias = null;
        }
    }

    public void SetPattern(SimulatedFramePattern pattern)
    {
        lock (_gate)
        {
            _pattern = pattern;
        }
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed)
            {
                return;
            }

            _timer?.Dispose();
            _timer = null;
            _activeCameraAlias = null;
            _disposed = true;
        }
    }

    private void OnTick(object? state)
    {
        string cameraAlias;
        SimulatedFramePattern pattern;
        int sequenceNumber;
        int generation;
        lock (_gate)
        {
            if (_activeCameraAlias is null)
            {
                return;
            }

            cameraAlias = _activeCameraAlias;
            pattern = _pattern;
            sequenceNumber = _sequenceNumber++;
            generation = _generation;
        }

        Tick?.Invoke(this, new SimulatedLiveViewFrameTick(cameraAlias, pattern, sequenceNumber, generation, _clock.UtcNow));
    }
}
