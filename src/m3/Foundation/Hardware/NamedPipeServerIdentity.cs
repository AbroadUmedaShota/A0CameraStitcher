using System.ComponentModel;
using System.IO.Pipes;
using System.Runtime.InteropServices;

namespace A0CameraStitcher.M3.Foundation.Hardware;

/// <summary>
/// Confirms that a connected named pipe client is talking to the exact server process the
/// caller spawned (GitHub Issue #85), using the Win32 GetNamedPipeServerProcessId query.
/// </summary>
/// <remarks>
/// PipeOptions.CurrentUserOnly (Issue #126/#127) already rules out a different Windows user
/// squatting on the pipe name before the real Agent creates it. It does not rule out a
/// same-user process doing the same thing: any process running as the current user can create a
/// pipe with a guessed or observed name first. The caller of this check holds a live
/// System.Diagnostics.Process handle for the exact PID it spawned and expects, so Windows cannot
/// have recycled that PID to an unrelated process while the handle stays open -- comparing it
/// against the connected pipe's actual server PID closes that remaining gap. Both a PID mismatch
/// and a failure to query the actual PID fail closed: the caller never treats a connection whose
/// identity it cannot prove as if it were the real Agent.
/// </remarks>
public static partial class NamedPipeServerIdentity
{
    /// <summary>
    /// No spawned host exists yet to compare against (DualBinding has no launcher as of Issue
    /// #85 -- see OperatorShellViewModel). A real named pipe server process ID is always a
    /// non-negative Win32 DWORD, so this negative sentinel can never match one: it keeps the
    /// connection fail-closed instead of silently trusting whichever process answered the pipe.
    /// </summary>
    public const int NoLauncherProcessId = -1;

    internal static void Verify(NamedPipeClientStream pipe, string pipeName, int expectedServerProcessId)
    {
        ArgumentNullException.ThrowIfNull(pipe);
        uint actualServerProcessId;
        bool succeeded;
        try
        {
            succeeded = GetNamedPipeServerProcessId(pipe.SafePipeHandle, out actualServerProcessId);
        }
        catch (Exception exception) when (
            exception is ObjectDisposedException or NotSupportedException or InvalidOperationException)
        {
            throw new HardwareCameraAgentServerIdentityException(
                pipeName, expectedServerProcessId, actualServerProcessId: null, exception);
        }

        if (!succeeded)
        {
            var win32Error = Marshal.GetLastPInvokeError();
            throw new HardwareCameraAgentServerIdentityException(
                pipeName,
                expectedServerProcessId,
                actualServerProcessId: null,
                new Win32Exception(win32Error));
        }

        if (actualServerProcessId > int.MaxValue || (int)actualServerProcessId != expectedServerProcessId)
        {
            throw new HardwareCameraAgentServerIdentityException(
                pipeName, expectedServerProcessId, (int)actualServerProcessId, innerException: null);
        }
    }

    [LibraryImport("kernel32.dll", EntryPoint = "GetNamedPipeServerProcessId", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool GetNamedPipeServerProcessId(SafeHandle pipeHandle, out uint serverProcessId);
}

/// <summary>
/// Thrown when a named pipe connection did succeed, but the server on the other end is not the
/// exact process the caller spawned (a mismatched PID) or its identity could not be determined
/// (a Win32 API failure). Both cases fail closed before any request byte is written, so this
/// derives from <see cref="HardwareCameraAgentConnectException"/>: every "confirmed
/// undispatched" check already written against that type (HardwareSingleCameraViewModel,
/// DualCameraAgentLifecycle, Persistent/ServeOnce operations) treats a server-identity failure
/// exactly the same as a connect failure, without those call sites needing to know this type
/// exists.
/// </summary>
public sealed class HardwareCameraAgentServerIdentityException : HardwareCameraAgentConnectException
{
    internal HardwareCameraAgentServerIdentityException(
        string pipeName,
        int expectedServerProcessId,
        int? actualServerProcessId,
        Exception? innerException)
        : base(
            actualServerProcessId.HasValue
                ? "The hardware Camera Agent pipe server process id did not match the process this client spawned."
                : "The hardware Camera Agent pipe server process id could not be verified.",
            pipeName,
            callerCancellationRequested: false,
            innerException)
    {
        ExpectedServerProcessId = expectedServerProcessId;
        ActualServerProcessId = actualServerProcessId;
    }

    public int ExpectedServerProcessId { get; }

    /// <summary>Null when the actual PID could not be queried at all, rather than mismatched.</summary>
    public int? ActualServerProcessId { get; }
}
