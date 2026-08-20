namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>
/// Production pump backed by <see cref="System.Threading.Timer"/> — a thread-pool timer,
/// not a WPF DispatcherTimer — so it carries no UI-thread affinity. This keeps the timer
/// out of OperatorShellViewModel: the ViewModel only reacts to <see cref="FrameProduced"/>
/// and marshals onto its own SynchronizationContext, which keeps headless ViewModel tests
/// free of any dependency on a Dispatcher or message loop.
/// </summary>
public sealed class SimulatedLiveViewFramePump : ISimulatedLiveViewFramePump
{
    private readonly ISimulatedLiveViewFrameSource _frameSource;
    private readonly ISimulatedFrameClock _clock;
    private readonly TimeSpan _interval;
    private readonly object _gate = new();
    private Timer? _timer;
    private string? _activeCameraAlias;
    private SimulatedFramePattern _pattern;
    private int _sequenceNumber;
    private bool _disposed;

    public SimulatedLiveViewFramePump(
        ISimulatedLiveViewFrameSource frameSource,
        ISimulatedFrameClock? clock = null,
        TimeSpan? interval = null)
    {
        _frameSource = frameSource ?? throw new ArgumentNullException(nameof(frameSource));
        _clock = clock ?? new SystemSimulatedFrameClock();
        _interval = interval ?? TimeSpan.FromMilliseconds(200);
        if (_interval <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(interval), "The frame interval must be positive.");
        }
    }

    public event EventHandler<SimulatedLiveViewFrame>? FrameProduced;

    public void Start(string cameraAlias, SimulatedFramePattern pattern)
    {
        if (string.IsNullOrWhiteSpace(cameraAlias))
        {
            throw new ArgumentException("A camera alias is required to start the SIMULATED frame pump.", nameof(cameraAlias));
        }

        ObjectDisposedException.ThrowIf(_disposed, this);
        lock (_gate)
        {
            _activeCameraAlias = cameraAlias;
            _pattern = pattern;
            _sequenceNumber = 0;
            _timer?.Dispose();
            _timer = new Timer(OnTick, null, TimeSpan.Zero, _interval);
        }
    }

    public void Stop()
    {
        Timer? timerToDispose;
        lock (_gate)
        {
            timerToDispose = _timer;
            _timer = null;
            _activeCameraAlias = null;
        }

        if (timerToDispose is null)
        {
            return;
        }

        // Dispose(WaitHandle) blocks until any callback that was already executing when
        // Stop() was called has finished, so once Stop() returns no frame will ever be
        // produced afterwards — the pump-level half of the "no frame supply while Live
        // View is OFF" contract (the ViewModel's alias/IsLiveViewActive check on apply is
        // the other half, guarding against a frame that was already in flight).
        using var disposedSignal = new ManualResetEvent(initialState: false);
        timerToDispose.Dispose(disposedSignal);
        disposedSignal.WaitOne(TimeSpan.FromSeconds(2));
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
        if (_disposed)
        {
            return;
        }

        Stop();
        _disposed = true;
    }

    private void OnTick(object? state)
    {
        string cameraAlias;
        SimulatedFramePattern pattern;
        int sequenceNumber;
        lock (_gate)
        {
            if (_activeCameraAlias is null)
            {
                return;
            }

            cameraAlias = _activeCameraAlias;
            pattern = _pattern;
            sequenceNumber = _sequenceNumber++;
        }

        // Rendered outside the lock: generation only touches locals plus the (stateless
        // per call) frame source, and holding the lock across a render would block Stop()
        // for the duration of a frame render.
        var frame = _frameSource.CreateFrame(cameraAlias, pattern, sequenceNumber, _clock.UtcNow);
        FrameProduced?.Invoke(this, frame);
    }
}
