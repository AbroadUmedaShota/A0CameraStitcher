using System.Text.Json;

namespace A0CameraStitcher.M3.Foundation;

public sealed class SimulatedCameraAgent
{
    public CameraAgentResponseEnvelope Handle(CameraAgentRequestEnvelope request)
    {
        return request.Operation switch
        {
            CameraAgentProtocol.Operations.Status => Success(
                request,
                "StatusReady",
                new
                {
                    marker = CameraAgentProtocol.Marker,
                    agentStatus = "Ready",
                    cameraAccess = "None",
                    realCameraApiCalls = 0,
                }),
            CameraAgentProtocol.Operations.Inventory => Success(
                request,
                "InventoryReady",
                new
                {
                    marker = CameraAgentProtocol.Marker,
                    physicalDeviceCount = 0,
                    devices = new[]
                    {
                        new { alias = "CAM-A", simulation = true, marker = CameraAgentProtocol.Marker },
                        new { alias = "CAM-B", simulation = true, marker = CameraAgentProtocol.Marker },
                    },
                }),
            CameraAgentProtocol.Operations.PreviewPlaceholder => Success(
                request,
                "PreviewPlaceholderReady",
                new
                {
                    marker = CameraAgentProtocol.Marker,
                    previewKind = "placeholder",
                    artifactExtension = ".simulated",
                    contentType = "text/plain",
                    cameraImage = false,
                }),
            _ => throw new ProtocolViolationException("UnsupportedOperation", "The request operation is not supported."),
        };
    }

    private static CameraAgentResponseEnvelope Success(
        CameraAgentRequestEnvelope request,
        string resultCode,
        object payload) =>
        CameraAgentProtocolCodec.CreateSuccess(
            request.RequestId,
            resultCode,
            JsonSerializer.SerializeToElement(payload));
}
