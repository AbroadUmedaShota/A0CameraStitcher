using System.IO;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

// Reconstructs the exact on-disk locations the hardware Camera Agent uses for
// a Single-camera run's artifacts, entirely from values this process already
// owns or has independently validated:
//   - artifactsRoot is the value this process itself passed to the agent via
//     --artifacts-root (see PersistentHardwareCameraAgentOperations /
//     ServeOnceHardwareCameraAgentOperations), so it is trusted, not agent-
//     reported.
//   - runId, transactionId, and cameraAlias come from the agent's response
//     payload, but HardwareCameraAgentProtocol already validates their shape
//     (run-<digits>-<digits>, 32 lowercase hex characters, CAM-A/CAM-B) and,
//     for transactionId, that it matches the transaction this process itself
//     requested -- before HardwareSingleCameraViewModel ever sees them.
//
// The result is compared for exact equality against the path the agent
// reports for a retained original or Live View preview (see
// HardwareArtifactVerifier.ValidateExpectedRecord). This is a provenance
// guarantee and accident detector, not an authentication boundary: the named
// pipe's ACL + CurrentUserOnly restriction is what establishes that the peer
// is this operator's own Camera Agent in the first place.
//
// Shapes mirror the native agent (hardware_camera_agent.cpp):
//   original: PersistCanonicalOriginal-style writers place the original at
//     <artifactsRoot>/<runId>/<transactionId>/<cameraAlias>/original.jpg
//     (IsCanonicalOriginalLocation requires exactly this 3-segment shape
//     relative to <artifactsRoot>/<runId>).
//   preview: PersistPreviewJpeg (hardware_camera_agent.cpp:915-930) places
//     Live View previews at
//     <artifactsRoot>/<runId>/live-view/<cameraAlias>/preview.jpg
internal static class HardwareAgentArtifactLayout
{
    internal static string OriginalPath(
        string artifactsRoot,
        string runId,
        string transactionId,
        string cameraAlias) =>
        Path.GetFullPath(Path.Combine(artifactsRoot, runId, transactionId, cameraAlias, "original.jpg"));

    internal static string PreviewPath(
        string artifactsRoot,
        string runId,
        string cameraAlias) =>
        Path.GetFullPath(Path.Combine(artifactsRoot, runId, "live-view", cameraAlias, "preview.jpg"));
}
