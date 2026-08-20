namespace A0CameraStitcher.M3.OperatorShell.Simulated;

/// <summary>Supplies the timestamp stamped onto each generated SIMULATED live view frame.</summary>
public interface ISimulatedFrameClock
{
    DateTimeOffset UtcNow { get; }
}

/// <summary>Production clock backed by the real system time.</summary>
public sealed class SystemSimulatedFrameClock : ISimulatedFrameClock
{
    public DateTimeOffset UtcNow => DateTimeOffset.UtcNow;
}
