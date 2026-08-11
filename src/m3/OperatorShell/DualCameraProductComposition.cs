using System.IO;
using A0CameraStitcher.M3.Foundation.DualCamera;

namespace A0CameraStitcher.M3.OperatorShell;

internal static class DualCameraProductComposition
{
    public static IDualCameraProductFlow Create(string artifactRoot)
    {
        var configuredPath = Environment.GetEnvironmentVariable("A0_M2_ADAPTER_PATH");
        var adapterPath = string.IsNullOrWhiteSpace(configuredPath)
            ? Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe")
            : Path.GetFullPath(configuredPath);
        if (!File.Exists(adapterPath))
        {
            return new UnavailableDualCameraProductFlow(adapterPath);
        }
        var adapter = new M2OfflineStitcherProcessAdapter(adapterPath);
        return new DualCameraProductFlow(
            artifactRoot,
            adapter,
            adapter,
            new FixedDualCameraIdentitySnapshotSource(
                DualCameraIdentitySnapshot.AnonymousTestSyntheticReady()));
    }

    private sealed class UnavailableDualCameraProductFlow(string expectedPath) : IDualCameraProductFlow
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

        public DualCameraIdentitySnapshot IdentitySnapshot => DualCameraIdentitySnapshot.HardwarePending();

        public Task<DualCameraProductState> CaptureAndStitchAsync(
            DualCameraCaptureRequest request,
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
