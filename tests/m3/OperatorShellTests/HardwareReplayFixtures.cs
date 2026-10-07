using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

// GitHub Issue #231: replay of anonymized real D810 responses.
//
// The fixtures under tests/fixtures/hardware-replay come from two real operator sessions (one
// single-camera capture, two dual-camera CaptureRecoveryOnly transactions). Everything that
// could identify a body, a person, a PC or a photograph was replaced; see the README next to
// the fixtures for the replacement rules and for which files are real bytes and which are
// reconstructed. The contracts below feed those responses to the app's single-camera and
// dual-camera save paths so that a difference between the fake used elsewhere in the suite and
// the real device cannot go unnoticed again (#216, #222, #224).
internal static class HardwareReplayFixtures
{
    internal const string SingleTransactionId = "f231a0010000000000000000000000a1";
    internal const string DualSucceededTransactionId = "f231b0020000000000000000000000b2";
    internal const string DualFailedTransactionId = "f231b0030000000000000000000000b3";
    internal const string SingleRunId = "run-231001-1";
    internal const string SyntheticPathRoot = "a0-replay-fixture";

    // The profile in the single-camera fixture expired in the (shifted) past by the time these
    // tests run, and the readiness codec compares against the wall clock. The replay rebases
    // only this one field to a far-future value; nothing else about the profile changes.
    internal const string RebasedProfileExpiry = "2099-01-31T05:04:50Z";
    internal const string FixtureProfileExpiry = "2026-01-31T05:04:50Z";

    internal static string Directory { get; } =
        Path.Combine(AppContext.BaseDirectory, "fixtures", "hardware-replay");

    internal static string ReadText(string relativePath) =>
        File.ReadAllText(Path.Combine(Directory, relativePath), Encoding.UTF8);

    internal static byte[] ReadJpeg(string name) =>
        Convert.FromBase64String(ReadText($"images/{name}.jpg.b64"));

    internal static string Sha256(byte[] bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    // The Agent's pair journal stores its terminal result as hex of the UTF-8 JSON.
    internal static string DecodeTerminalResult(string journalText)
    {
        using var document = JsonDocument.Parse(journalText);
        var hex = document.RootElement.GetProperty("terminalResultHex").GetString()!;
        return Encoding.UTF8.GetString(Convert.FromHexString(hex));
    }

    internal static string DualTerminalResult(ReplayDualScenario scenario) =>
        DecodeTerminalResult(ReadText($"dual-camera/{ScenarioFolder(scenario)}/pair-journal.json"));

    internal static string ScenarioFolder(ReplayDualScenario scenario) => scenario switch
    {
        ReplayDualScenario.Succeeded => "succeeded",
        ReplayDualScenario.FailedCameraA => "failed-cam-a",
        _ => throw new ArgumentOutOfRangeException(nameof(scenario)),
    };

    internal static string ScenarioTransactionId(ReplayDualScenario scenario) => scenario switch
    {
        ReplayDualScenario.Succeeded => DualSucceededTransactionId,
        ReplayDualScenario.FailedCameraA => DualFailedTransactionId,
        _ => throw new ArgumentOutOfRangeException(nameof(scenario)),
    };

    internal static string NewTestRoot()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            "A0CameraStitcher-M3-HardwareReplayTests",
            Guid.NewGuid().ToString("N"));
        System.IO.Directory.CreateDirectory(root);
        return root;
    }

    internal static void DeleteTestRoot(string root)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                if (System.IO.Directory.Exists(root)) System.IO.Directory.Delete(root, recursive: true);
                return;
            }
            catch (IOException) when (attempt < 10)
            {
                Thread.Sleep(50);
            }
        }
    }
}

internal enum ReplayDualScenario
{
    Succeeded,
    FailedCameraA,
}

internal static class HardwareReplayAnonymizationRules
{
    private static readonly string[] AllowedExtensions = [".md", ".json", ".jsonl", ".b64"];

    // Forbidden anywhere in a data file. Documentation (README.md) is exempt from the word rules
    // only, because it has to say which categories were removed.
    private static readonly (string Name, Regex Pattern)[] DataFileRules =
    [
        ("user profile path", new Regex(@"[\\/]users[\\/]", RegexOptions.IgnoreCase)),
        ("app data folder", new Regex("appdata", RegexOptions.IgnoreCase)),
        ("serial number", new Regex("serial", RegexOptions.IgnoreCase)),
        ("USB or PnP identifier", new Regex(@"\b(vid|pid)_[0-9a-f]{4}|usb[\\#]|\\\\\?\\|device ?id|instance ?id|pnp", RegexOptions.IgnoreCase)),
        ("windows account or host name", new Regex(@"\b[A-Z]{2,6}-\d{2}-NOTE\b|\buser name\b", RegexOptions.IgnoreCase)),
    ];

    private static readonly Regex HexRun = new(
        "(?<![0-9a-fA-F])[0-9a-fA-F]{16,}(?![0-9a-fA-F])", RegexOptions.Compiled);
    private static readonly Regex AbsolutePath = new(@"[A-Za-z]:[\\/]+", RegexOptions.Compiled);
    private static readonly Regex Epoch = new(@"(?<!\d)1[5-9]\d{11}(?!\d)", RegexOptions.Compiled);
    private static readonly Regex DateText = new(@"\d{4}-\d{2}-\d{2}", RegexOptions.Compiled);

    internal static IReadOnlyList<string> CheckFileSet(string root)
    {
        var problems = new List<string>();
        foreach (var path in System.IO.Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(root, path).Replace('\\', '/');
            if (!AllowedExtensions.Contains(Path.GetExtension(path), StringComparer.OrdinalIgnoreCase))
                problems.Add($"{relative}: file type is not allowed in the replay fixtures (images are stored only as .jpg.b64 dummies)");
            if (Path.GetFileName(path).EndsWith(".jpg", StringComparison.OrdinalIgnoreCase) ||
                Path.GetFileName(path).EndsWith(".jpeg", StringComparison.OrdinalIgnoreCase))
                problems.Add($"{relative}: a real image file must never be stored here");
        }
        return problems;
    }

    internal static IReadOnlyList<string> Scan(
        string name,
        string text,
        IReadOnlySet<string> allowedSha256,
        bool isDocumentation)
    {
        var problems = new List<string>();
        if (name.EndsWith(".b64", StringComparison.Ordinal)) return problems;

        text = ExpandTerminalResultHex(text);

        foreach (Match match in AbsolutePath.Matches(text))
        {
            var tail = text[(match.Index + match.Length)..];
            if (!tail.StartsWith(HardwareReplayFixtures.SyntheticPathRoot, StringComparison.Ordinal))
                problems.Add($"{name}: absolute path outside the synthetic root at offset {match.Index}");
        }

        if (!isDocumentation)
        {
            foreach (var (ruleName, pattern) in DataFileRules)
            {
                if (pattern.IsMatch(text)) problems.Add($"{name}: contains a {ruleName}");
            }
        }

        foreach (Match match in HexRun.Matches(text))
        {
            var value = match.Value.ToLowerInvariant();
            var allowed = value.Length switch
            {
                32 => value.StartsWith("f231", StringComparison.Ordinal),
                64 => value.StartsWith("f231", StringComparison.Ordinal) || allowedSha256.Contains(value),
                _ => false,
            };
            if (!allowed)
                problems.Add($"{name}: hexadecimal run of {value.Length} characters is not a synthetic value");
        }

        if (Epoch.IsMatch(text)) problems.Add($"{name}: contains a 13-digit epoch timestamp (real run id shape)");

        foreach (Match match in DateText.Matches(text))
        {
            if (!match.Value.StartsWith("2026-01-", StringComparison.Ordinal))
                problems.Add($"{name}: date {match.Value} is outside the shifted fixture month");
        }
        return problems;
    }

    // The pair journal keeps the Agent's terminal result as one long hex string. Scan the text it
    // encodes, not the opaque hex.
    private static string ExpandTerminalResultHex(string text) =>
        Regex.Replace(
            text,
            "\"terminalResultHex\"\\s*:\\s*\"([0-9a-fA-F]+)\"",
            match => "\"terminalResultDecoded\":" + Encoding.UTF8.GetString(Convert.FromHexString(match.Groups[1].Value)));
}

internal static class HardwareReplayJpegContracts
{
    internal static void RequireDummyJpeg(string name, byte[] bytes)
    {
        Check.True(bytes.Length is > 100 and <= 4096,
            $"{name}: a replay dummy JPEG must be a few hundred bytes, got {bytes.Length}.");
        Check.True(bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[^2] == 0xFF && bytes[^1] == 0xD9,
            $"{name}: dummy JPEG must have SOI and EOI markers.");

        var sawComment = false;
        var sawFrame = false;
        for (var index = 2; index + 4 < bytes.Length;)
        {
            Check.True(bytes[index] == 0xFF, $"{name}: JPEG marker structure is invalid.");
            var marker = bytes[index + 1];
            if (marker is 0xD9 or 0xDA) break;
            var length = (bytes[index + 2] << 8) | bytes[index + 3];
            Check.False(marker == 0xE1,
                $"{name}: an APP1 (EXIF/XMP) segment must never appear in a dummy image.");
            if (marker == 0xFE)
            {
                var comment = Encoding.ASCII.GetString(bytes, index + 4, length - 2);
                Check.True(comment.Contains("synthetic image, not a photograph", StringComparison.Ordinal),
                    $"{name}: the dummy JPEG must carry the synthetic-image notice.");
                sawComment = true;
            }
            if (marker == 0xC0)
            {
                var height = (bytes[index + 5] << 8) | bytes[index + 6];
                var width = (bytes[index + 7] << 8) | bytes[index + 8];
                Check.True(width == 7360 && height == 4912,
                    $"{name}: dummy JPEG must declare the D810 L size 7360x4912, got {width}x{height}.");
                sawFrame = true;
            }
            index += 2 + length;
        }
        Check.True(sawComment && sawFrame, $"{name}: dummy JPEG lacks its notice or frame header.");
    }
}

// Replays the Agent that owns the dual-camera CaptureRecoveryOnly transaction. The wire
// envelopes are synthesized (the Agent's raw responses are not kept on disk); the "result" object
// is the Agent's real terminal result, decoded from its real pair journal. Like the real Agent it
// writes the retained originals to <transaction directory>/<alias>/original.jpg.
internal sealed class ReplayDualHardwareTransport(
    ReplayDualScenario scenario,
    bool rebaseTimesToRequest = false,
    bool startResponseUnknown = false,
    bool firstQueryFails = false) : IHardwareCameraAgentTransport
{
    private bool _queryFailureSpent;
    private static readonly JsonSerializerOptions EnvelopeOptions = new(JsonSerializerDefaults.Web);

    private Guid? _transactionId;
    private JsonElement? _terminal;

    public List<string> Operations { get; } = [];

    public Task<string> SendAsync(string requestJson, CancellationToken cancellationToken = default)
    {
        using var document = JsonDocument.Parse(requestJson);
        var root = document.RootElement;
        var requestId = root.GetProperty("requestId").GetString()!;
        var operation = root.GetProperty("operation").GetString()!;
        Operations.Add(operation);
        var payload = root.GetProperty("payload");
        return Task.FromResult(operation switch
        {
            DualHardwareCameraAgentProtocol.Operations.GetCapabilities =>
                BuildEnvelope(requestId, "DualCapabilities", new
                {
                    cameraMode = "DualCamera",
                    protocolVersion = 2,
                    orderedRequiredAliases = new[] { "CAM-A", "CAM-B" },
                    supportedOperations = DualHardwareCameraAgentProtocol.Operations.Required
                        .Concat([DualHardwareCameraAgentProtocol.Operations.StartReservedCaptureRecoveryOnly])
                        .ToArray(),
                    pairJournalDurable = true,
                    sameTransactionQueryOnly = true,
                    automaticRetryCount = 0,
                }, DualHardwareCameraAgentProtocol.SchemaVersion),
            DualHardwareCameraAgentProtocol.Operations.ReservePairTransaction =>
                BuildEnvelope(requestId, "PairTransactionReserved", new
                {
                    transactionId = payload.GetProperty("transactionId").GetString(),
                    accepted = true,
                }, DualHardwareCameraAgentProtocol.SchemaVersion),
            DualHardwareCameraAgentProtocol.Operations.StartReservedCaptureRecoveryOnly =>
                Start(requestId, payload.GetProperty("transaction")),
            DualHardwareCameraAgentProtocol.Operations.GetPairTransactionResult =>
                Query(requestId, payload),
            DualHardwareCameraAgentProtocol.Operations.CloseReservedPairTransaction =>
                BuildEnvelope(requestId, "PairTransactionClosedBeforeDispatch", new
                {
                    transactionId = payload.GetProperty("transactionId").GetString(),
                    closedBeforeDispatch = true,
                }, DualHardwareCameraAgentProtocol.CaptureRecoveryOnlySchemaVersion),
            _ => throw new InvalidOperationException($"The replay transport does not model operation '{operation}'."),
        });
    }

    private string Start(string requestId, JsonElement transaction)
    {
        var transactionId = Guid.ParseExact(transaction.GetProperty("transactionId").GetString()!, "N");
        var transactionDirectory = transaction.GetProperty("transactionDirectory").GetString()!;
        _transactionId = transactionId;
        _terminal = BuildTerminal(transactionId, transactionDirectory, transaction);
        // The real Agent has already finished (and written the originals) by the time the app
        // learns the outcome, including when the app never sees this response.
        return BuildEnvelope(requestId, "PairDispatchAccepted", new
        {
            transactionId = transactionId.ToString("N"),
            dispatchState = startResponseUnknown ? "ResponseUnknown" : "Completed",
            result = startResponseUnknown ? (JsonElement?)null : _terminal,
        }, DualHardwareCameraAgentProtocol.CaptureRecoveryOnlySchemaVersion);
    }

    private string Query(string requestId, JsonElement payload)
    {
        if (firstQueryFails && !_queryFailureSpent)
        {
            // The pipe broke after the request left: the app does not learn the outcome yet.
            _queryFailureSpent = true;
            throw new IOException("replay: transport failure while querying the transaction");
        }
        var requested = payload.GetProperty("transactionId").GetString()!;
        if (_transactionId is null || _terminal is null ||
            !string.Equals(requested, _transactionId.Value.ToString("N"), StringComparison.Ordinal))
            throw new InvalidOperationException("The replay Agent was asked about a transaction it never started.");
        return BuildEnvelope(requestId, "PairTransactionFound", new
        {
            transactionId = requested,
            found = true,
            result = _terminal,
        }, DualHardwareCameraAgentProtocol.CaptureRecoveryOnlySchemaVersion);
    }

    private JsonElement BuildTerminal(Guid transactionId, string transactionDirectory, JsonElement requestTransaction)
    {
        var fixtureTransactionId = HardwareReplayFixtures.ScenarioTransactionId(scenario);
        var actualId = transactionId.ToString("N");
        var text = HardwareReplayFixtures.DualTerminalResult(scenario)
            .Replace(
                $"C:/{HardwareReplayFixtures.SyntheticPathRoot}/dual-camera-hardware-products/transactions/{fixtureTransactionId}",
                transactionDirectory.Replace('\\', '/'),
                StringComparison.Ordinal)
            .Replace(fixtureTransactionId, actualId, StringComparison.Ordinal);

        var node = JsonNode.Parse(text)!;
        if (rebaseTimesToRequest)
        {
            // Only for scenarios where the app creates its own clock readings (the shell derives
            // the identity snapshot from its own binding). The interval between the Agent's own
            // timestamps is kept.
            var evidence = node["evidence"]!.AsObject();
            var fixtureStarted = DateTimeOffset.Parse(evidence["watchdogStartedAtUtc"]!.GetValue<string>());
            var fixtureCompleted = DateTimeOffset.Parse(evidence["completedAtUtc"]!.GetValue<string>());
            var started = requestTransaction.GetProperty("startedAtUtc").GetString()!;
            var startedValue = DateTimeOffset.Parse(started);
            evidence["identitySnapshot"] = JsonNode.Parse(requestTransaction.GetProperty("identitySnapshot").GetRawText());
            evidence["watchdogStartedAtUtc"] = started;
            evidence["watchdogDeadlineUtc"] = requestTransaction.GetProperty("watchdogDeadlineUtc").GetString();
            evidence["completedAtUtc"] = (startedValue + (fixtureCompleted - fixtureStarted)).ToString("O");
        }

        var result = JsonSerializer.SerializeToElement(node);
        if (scenario == ReplayDualScenario.Succeeded)
        {
            WriteOriginal(transactionDirectory, "CAM-A", HardwareReplayFixtures.ReadJpeg("dual-cam-a"));
            WriteOriginal(transactionDirectory, "CAM-B", HardwareReplayFixtures.ReadJpeg("dual-cam-b"));
        }
        return result;
    }

    private static void WriteOriginal(string transactionDirectory, string alias, byte[] bytes)
    {
        var path = Path.Combine(transactionDirectory, alias, "original.jpg");
        System.IO.Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, bytes);
    }

    private static string BuildEnvelope(string requestId, string resultCode, object payload, string schemaVersion) =>
        JsonSerializer.Serialize(
            new
            {
                schemaVersion,
                simulation = false,
                marker = DualHardwareCameraAgentProtocol.Marker,
                requestId,
                success = true,
                resultCode,
                payload,
            },
            EnvelopeOptions);
}

// Where the replayed single-camera Agent lands the original.
internal enum ReplaySingleLanding
{
    // <run>/<app transaction ID>/<alias>/original.jpg, what the real Agent does since #216.
    AppTransactionFolder,

    // <run>/hybrid-tx-<id>/<alias>/original.jpg, what the Agent did before #216 and what the
    // field run behind #216 showed to the app as original_reread_failed.
    LegacyHybridFolder,
}

// Replays the single-camera Agent. The capture response is the real transaction journal mapped to
// the wire payload (see the fixture README); readiness is built from the real approved profile.
// Every response goes through the production codec, so a shape the codec cannot read fails here.
internal sealed class ReplaySingleCameraOperations(
    string artifactsRoot,
    ReplaySingleLanding landing) : IHardwareSingleCameraOperations
{
    internal const string ReadinessRequestId = "req-replay-readiness";
    internal const string CaptureRequestId = "req-replay-1";

    public string AgentExecutablePath => Path.Combine(artifactsRoot, "..", "..", "app", "fake-agent.exe");

    public string AgentArtifactsRoot => artifactsRoot;

    public bool AgentExecutableAvailable => true;

    public int CaptureCalls { get; private set; }

    public string? LastOriginalPath { get; private set; }

    public Task<HardwareCameraAgentReply<HardwareSingleReadinessResult>> GetReadinessAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default) =>
        Task.FromResult(HardwareCameraAgentProtocolCodec.DeserializeReadinessResponse(
            ReadinessJson(), ReadinessRequestId, cameraAlias));

    public Task<HardwareCameraAgentReply<HardwareSingleLiveViewResult>> ProbeLiveViewAsync(
        string cameraAlias,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The replay does not model Live View.");

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> CaptureAsync(
        string transactionId,
        string cameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool liveViewHandoffRequested,
        CancellationToken cancellationToken = default)
    {
        CaptureCalls++;
        var json = CaptureJson(transactionId);
        return Task.FromResult(HardwareCameraAgentProtocolCodec.DeserializeCaptureResponse(
            json, CaptureRequestId, transactionId, cameraAlias, expectedProfile, liveViewHandoffRequested));
    }

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The replay never needs a historical lookup.");

    public Task<HardwareCameraAgentReply<HardwareSingleCaptureResult>> GetTransactionResultAsync(
        string transactionId,
        string expectedCameraAlias,
        HardwareCaptureProfileSnapshot expectedProfile,
        bool expectedLiveViewHandoffRequested,
        CancellationToken cancellationToken = default) =>
        throw new InvalidOperationException("The replay never needs a recovery lookup.");

    // The real approved profile carries the nine observed settings of the real D810, including
    // fileType = {"available": false}. The remaining descriptor fields follow the Agent's own
    // serialization for a capability the SDK does not advertise.
    internal static string ObservedSettingsJson()
    {
        using var profile = JsonDocument.Parse(
            HardwareReplayFixtures.ReadText("single-camera/approved-capture-profile.json"));
        var expected = profile.RootElement.GetProperty("expectedSettings");
        var builder = new StringBuilder("{");
        var first = true;
        foreach (var setting in expected.EnumerateObject())
        {
            if (!first) builder.Append(',');
            first = false;
            builder.Append('"').Append(setting.Name).Append("\":");
            builder.Append(ObservedSettingJson(setting.Value));
        }
        return builder.Append('}').ToString();
    }

    private static string ObservedSettingJson(JsonElement setting)
    {
        string Text(string name, string fallback) =>
            setting.TryGetProperty(name, out var value) ? JsonSerializer.Serialize(value.GetString()) : JsonSerializer.Serialize(fallback);
        string Number(string name) =>
            setting.TryGetProperty(name, out var value) ? value.GetRawText() : "null";
        string Label() =>
            setting.TryGetProperty("currentLabel", out var value) ? value.GetRawText() : "null";
        var available = setting.GetProperty("available").GetBoolean();
        return "{\"available\":" + (available ? "true" : "false") +
            ",\"capType\":" + Text("capType", "unsupported") +
            ",\"probeState\":" + Text("probeState", "not-advertised") +
            ",\"valueType\":" + Text("valueType", "unsupported") +
            ",\"currentValue\":" + Number("currentValue") +
            ",\"currentIndex\":" + Number("currentIndex") +
            ",\"currentLabel\":" + Label() + "}";
    }

    private static string ReadinessJson()
    {
        using var transaction = JsonDocument.Parse(
            HardwareReplayFixtures.ReadText("single-camera/transaction.json"));
        var root = transaction.RootElement;
        string Raw(string name) => root.GetProperty(name).GetRawText();
        return "{\"schemaVersion\":\"a0.camera-agent.hardware.v1\",\"simulation\":false,\"marker\":\"Hardware\"," +
            "\"requestId\":\"" + ReadinessRequestId + "\",\"success\":true,\"resultCode\":\"SingleReady\",\"payload\":{" +
            "\"cameraMode\":\"SingleCamera\",\"cameraAlias\":\"CAM-A\",\"ready\":true,\"sdkCameraCount\":1,\"wpdCameraCount\":1," +
            "\"sdkIdentityBound\":true,\"wpdIdentityBound\":true,\"sdkAliasMatches\":true,\"wpdAliasMatches\":true," +
            "\"sdkStatusProbed\":true,\"spoolInspected\":true,\"spoolPayloadObjectCount\":0,\"spoolKnownEmpty\":true," +
            "\"firmware\":\"unknown\",\"liveViewStatus\":\"off\",\"liveViewStatusAvailable\":true," +
            "\"captureProfileApproved\":true,\"captureProfileId\":" + Raw("captureProfileId") +
            ",\"captureProfileVersion\":" + Raw("captureProfileVersion") +
            ",\"captureProfileSha256\":" + Raw("captureProfileSha256") +
            ",\"captureProfileCameraAlias\":" + Raw("captureProfileCameraAlias") +
            ",\"profileExpiresAtUtc\":\"" + HardwareReplayFixtures.RebasedProfileExpiry + "\"" +
            ",\"captureProfileAliasMatches\":true,\"settingsMatchApprovedProfile\":true," +
            "\"observedSettings\":" + ObservedSettingsJson() +
            ",\"readOnly\":true,\"captureCommandSent\":false,\"cameraObjectDeleteAttempted\":false," +
            "\"cameraSettingsChanged\":false,\"realIdentifiersIncluded\":false,\"failureCategory\":\"\",\"failureDetail\":\"\"}}";
    }

    private string CaptureJson(string appTransactionId)
    {
        var fixtureText = HardwareReplayFixtures.ReadText("single-camera/agent-capture-response.json");
        var fixtureRoot = $"C:\\{HardwareReplayFixtures.SyntheticPathRoot}\\phase0\\camera-agent\\artifacts";
        var folder = landing == ReplaySingleLanding.AppTransactionFolder
            ? appTransactionId
            : LegacyHybridFolderName();
        var originalPath = Path.GetFullPath(Path.Combine(
            artifactsRoot, HardwareReplayFixtures.SingleRunId, folder, "CAM-A", "original.jpg"));
        System.IO.Directory.CreateDirectory(Path.GetDirectoryName(originalPath)!);
        File.WriteAllBytes(originalPath, HardwareReplayFixtures.ReadJpeg("single-cam-a"));
        LastOriginalPath = originalPath;

        // JSON text replacement on the raw envelope keeps every other byte exactly as fixed.
        var fixturePath = $"{fixtureRoot}\\{HardwareReplayFixtures.SingleRunId}\\{HardwareReplayFixtures.SingleTransactionId}\\CAM-A\\original.jpg";
        var escapedFixturePath = JsonEncodedText.Encode(fixturePath).ToString();
        var escapedActualPath = JsonEncodedText.Encode(originalPath).ToString();

        return fixtureText
            .Replace(escapedFixturePath, escapedActualPath, StringComparison.Ordinal)
            .Replace(HardwareReplayFixtures.SingleTransactionId, appTransactionId, StringComparison.Ordinal)
            .Replace(HardwareReplayFixtures.FixtureProfileExpiry, HardwareReplayFixtures.RebasedProfileExpiry, StringComparison.Ordinal);
    }

    // The pre-#216 folder name is taken from the real diagnostics of the dual legs, which still use
    // the executor's own "hybrid-tx-" ID.
    private static string LegacyHybridFolderName()
    {
        foreach (var line in HardwareReplayFixtures
                     .ReadText("dual-camera/succeeded/diagnostics-cam-a.events.jsonl")
                     .Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            using var document = JsonDocument.Parse(line);
            if (document.RootElement.TryGetProperty("transactionId", out var id) &&
                id.GetString() is { } value && value.StartsWith("hybrid-tx-", StringComparison.Ordinal))
                return value;
        }
        throw new InvalidDataException("The dual diagnostics fixture has no hybrid-tx transaction ID.");
    }
}
