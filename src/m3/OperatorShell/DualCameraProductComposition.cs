using System.IO;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.OperatorShell.Hardware;

namespace A0CameraStitcher.M3.OperatorShell;

internal static class DualCameraProductComposition
{
    public static IDualCameraProductFlow Create(
        string artifactRoot,
        DualCameraExecutionEnvironment environment = DualCameraExecutionEnvironment.TestSynthetic,
        IDualHardwareCaptureOperations? hardwareOperations = null,
        IDualCameraIdentitySnapshotSource? hardwareIdentitySource = null)
    {
        var configuredPath = Environment.GetEnvironmentVariable("A0_M2_ADAPTER_PATH");
        var candidatePath = string.IsNullOrWhiteSpace(configuredPath)
            ? Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe")
            : configuredPath;
        string adapterPath;
        try
        {
            // 環境変数由来のパスも既定パスも同じローカルEXE検証を通す。無効/不在なら
            // DualCamera を無効化する (SingleCamera や平文シミュレーションへはフォールバックしない)。
            adapterPath = CameraAgentExecutablePolicy.ResolveLocalExecutable(candidatePath);
        }
        catch (ArgumentException)
        {
            return new UnavailableDualCameraProductFlow(candidatePath, environment);
        }
        var adapter = new M2OfflineStitcherProcessAdapter(adapterPath);
        return environment switch
        {
            DualCameraExecutionEnvironment.TestSynthetic => new DualCameraProductFlow(
                artifactRoot,
                adapter,
                adapter,
                new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.AnonymousTestSyntheticReady())),
            DualCameraExecutionEnvironment.HardwareDual => CreateHardwareDual(
                artifactRoot,
                hardwareOperations,
                adapter,
                hardwareIdentitySource),
            _ => throw new ArgumentOutOfRangeException(nameof(environment)),
        };
    }

    private static IDualCameraProductFlow CreateHardwareDual(
        string artifactRoot,
        IDualHardwareCaptureOperations? hardwareOperations,
        IOfflineStitcherAdapter adapter,
        IDualCameraIdentitySnapshotSource? hardwareIdentitySource)
    {
        var recoveryStore = new HardwareDualTransactionSnapshotStore(artifactRoot);
        return new DualCameraProductFlow(
            artifactRoot,
            new HardwareDualCaptureSource(hardwareOperations, recoveryStore: recoveryStore),
            adapter,
            hardwareIdentitySource ?? new FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot.HardwarePending()));
    }

    private sealed class UnavailableDualCameraProductFlow(
        string expectedPath,
        DualCameraExecutionEnvironment environment) : IDualCameraProductFlow
    {
        public event EventHandler<DualCameraProductState>? StateChanged
        {
            add { }
            remove { }
        }

        public event EventHandler<DualCameraIdentitySnapshot>? IdentityChanged
        {
            add { }
            remove { }
        }

        public DualCameraProductState? Current => null;

        public DualCameraExecutionEnvironment ExecutionEnvironment => environment;

        public DualCameraIdentitySnapshot IdentitySnapshot => DualCameraIdentitySnapshot.HardwarePending();

        public Task<DualCameraProductState> CaptureAndStitchAsync(
            DualCameraCaptureRequest request,
            CancellationToken cancellationToken = default) =>
            Task.FromException<DualCameraProductState>(Unavailable());

        public Task<DualCameraProductState> RecoverAndStitchAsync(
            Guid transactionId,
            CancellationToken cancellationToken = default) =>
            Task.FromException<DualCameraProductState>(Unavailable());

        public Task<DualCameraProductState> RestitchAsync(CancellationToken cancellationToken = default) =>
            Task.FromException<DualCameraProductState>(Unavailable());

        public Task<DualCameraProductState> ExportAsync(
            string fixedLocalDirectory,
            CancellationToken cancellationToken = default) =>
            Task.FromException<DualCameraProductState>(Unavailable());

        private FileNotFoundException Unavailable() => new(
            "M2 offline stitcher adapter is unavailable; DualCamera does not fall back to SingleCamera or plain-text simulation.",
            expectedPath);
    }
}
