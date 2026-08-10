namespace A0CameraStitcher.M3.OperatorShell.Hardware;

public sealed class HardwareSingleAppSessionBusyException : Exception
{
    public HardwareSingleAppSessionBusyException()
        : base("同じWindowsログオンセッションで別の実機一台画面が開いています。")
    {
    }
}

public sealed class HardwareSingleAppSessionLease : IDisposable
{
    private const string LeaseName = @"Local\A0CameraStitcher.HardwareSingle.OperatorSession.v1";
    private readonly Mutex _mutex;
    private bool _owned;

    private HardwareSingleAppSessionLease(Mutex mutex, bool recoveredAbandonedOwner)
    {
        _mutex = mutex;
        _owned = true;
        RecoveredAbandonedOwner = recoveredAbandonedOwner;
    }

    public bool RecoveredAbandonedOwner { get; }

    public static HardwareSingleAppSessionLease Acquire()
    {
        var mutex = new Mutex(initiallyOwned: false, LeaseName);
        try
        {
            if (!mutex.WaitOne(TimeSpan.Zero))
            {
                mutex.Dispose();
                throw new HardwareSingleAppSessionBusyException();
            }

            return new HardwareSingleAppSessionLease(mutex, recoveredAbandonedOwner: false);
        }
        catch (AbandonedMutexException)
        {
            return new HardwareSingleAppSessionLease(mutex, recoveredAbandonedOwner: true);
        }
        catch
        {
            mutex.Dispose();
            throw;
        }
    }

    public void Dispose()
    {
        if (!_owned)
        {
            return;
        }

        _mutex.ReleaseMutex();
        _owned = false;
        _mutex.Dispose();
    }
}
