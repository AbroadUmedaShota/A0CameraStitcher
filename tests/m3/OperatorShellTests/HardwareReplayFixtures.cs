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

// Shape checks for the replay fixtures (Issue #231, hardened by #241).
//
// This is only a check of shapes. A value whose shape is unremarkable (a 7-character body serial,
// an approval GUID in a neutral field) cannot be found here; before anything is pushed, the
// fixtures are also compared against the real values on the operator's PC with
// scripts/Test-ReplayFixtureLeak.ps1 (see the fixture README). No real value, and no hash of one,
// is kept in the repository: the only machine-specific values used below are read from the
// environment while the test runs.
internal static class HardwareReplayAnonymizationRules
{
    private static readonly string[] AllowedExtensions = [".md", ".json", ".jsonl", ".b64"];

    // Base64 files are dummy images and nothing else: images/<name>.jpg.b64, directly in images/.
    private static readonly Regex ImageB64Name = new(@"^images/[^/]+\.jpg\.b64$", RegexOptions.Compiled);

    // Forbidden anywhere in a data file. Documentation (README.md) is exempt from the word rules
    // only, because it has to say which categories were removed.
    private static readonly (string Name, Regex Pattern)[] DataFileRules =
    [
        ("user profile path", new Regex(@"[\\/]users[\\/]", RegexOptions.IgnoreCase)),
        ("app data folder", new Regex("appdata", RegexOptions.IgnoreCase)),
        ("serial number", new Regex("serial", RegexOptions.IgnoreCase)),
        ("USB or PnP identifier", new Regex(@"\b(vid|pid)_[0-9a-f]{4}|usb[\\#]|\\\\\?\\|device ?id|instance ?id|pnp", RegexOptions.IgnoreCase)),
        ("windows account or host name", new Regex(@"\b[A-Z]{2,6}-\d{2}-NOTE\b|\buser name\b", RegexOptions.IgnoreCase)),
        // Windows' own default names (DESKTOP-xxxxxxx and the like). Case-sensitive on purpose:
        // "win-x64" style runtime identifiers are not host names.
        ("default windows host name", new Regex(@"\b(?:DESKTOP|LAPTOP|WIN)-[A-Z0-9]{5,}\b")),
        ("host, machine, account or owner field", new Regex(
            "\"[\\w\\-]*(?:computer|machine|host|pc|user|account|login)[\\w\\-]*name[\\w\\-]*\"\\s*:|\"[\\w\\-]*owner[\\w\\-]*\"\\s*:",
            RegexOptions.IgnoreCase)),
    ];

    private static readonly Regex HexRun = new(
        "(?<![0-9a-fA-F])[0-9a-fA-F]{16,}(?![0-9a-fA-F])", RegexOptions.Compiled);
    private static readonly Regex DashedGuid = new(
        "(?<![0-9a-fA-F])[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}(?![0-9a-fA-F])",
        RegexOptions.Compiled);
    private static readonly Regex AbsolutePath = new(@"[A-Za-z]:[\\/]+", RegexOptions.Compiled);
    private static readonly Regex EmailAddress = new(
        @"[A-Za-z0-9._%+\-]+@[A-Za-z0-9\-]+(?:\.[A-Za-z0-9\-]+)*\.[A-Za-z]{2,}", RegexOptions.Compiled);

    // Unix epoch in seconds (10 digits), milliseconds (13), microseconds (16) or nanoseconds (19),
    // 2017 to 2033. The old 13-digit rule is the milliseconds case.
    private static readonly Regex Epoch = new(@"(?<!\d)1[5-9]\d{8}(?:\d{3}|\d{6}|\d{9})?(?!\d)", RegexOptions.Compiled);

    // yyyyMMdd, yyyy-MM-dd, yyyy/MM/dd, yyyy.MM.dd and the EXIF form yyyy:MM:dd (same separator
    // twice, or none). Only January 2026, the month the fixtures were shifted into, is allowed.
    private static readonly Regex DateNumeric = new(
        @"(?<!\d)((?:19|20)\d{2})([-/:.]?)(0[1-9]|1[0-2])\2(0[1-9]|[12]\d|3[01])(?!\d)", RegexOptions.Compiled);
    private static readonly Regex DateJapanese = new(
        @"(?<!\d)((?:19|20)\d{2})年(\d{1,2})月(\d{1,2})日", RegexOptions.Compiled);

    // A bare alphanumeric string value with a digit in it (a serial number under a neutral key
    // name, an ID, a token). Keys are excluded by the lookahead. Explicit allow-list below.
    private static readonly Regex OpaqueTokenValue = new("\"([0-9A-Za-z]{5,24})\"(?!\\s*:)", RegexOptions.Compiled);
    private static readonly HashSet<string> AllowedTokenValues = new(StringComparer.Ordinal) { "7360x4912" };

    // A bare run of 6 or more digits (7-digit body serials, long counters). Not part of a longer
    // word, decimal fraction or hyphenated ID.
    private static readonly Regex LongDigitRun = new(@"(?<![\w.\-])\d{6,}(?![\w.\-])", RegexOptions.Compiled);

    // The synthetic 32/64-digit values the fixtures use: "f231", four free digits, a run of zeros,
    // two to four free digits. The whole shape is checked, not the prefix, so a real-looking value
    // that merely begins with "f231" is rejected.
    private static readonly Regex SyntheticId = new("^f231[0-9a-f]{4}0+[0-9a-f]{2,4}$", RegexOptions.Compiled);

    internal static IReadOnlyList<string> CheckFileSet(string root)
    {
        var problems = new List<string>();
        foreach (var path in System.IO.Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(root, path).Replace('\\', '/');
            var extension = Path.GetExtension(path);
            if (!AllowedExtensions.Contains(extension, StringComparer.OrdinalIgnoreCase))
                problems.Add($"{relative}: file type is not allowed in the replay fixtures (images are stored only as .jpg.b64 dummies)");
            if (Path.GetFileName(path).EndsWith(".jpg", StringComparison.OrdinalIgnoreCase) ||
                Path.GetFileName(path).EndsWith(".jpeg", StringComparison.OrdinalIgnoreCase))
                problems.Add($"{relative}: a real image file must never be stored here");
            var isImageFolder = relative.StartsWith("images/", StringComparison.OrdinalIgnoreCase);
            if ((extension.Equals(".b64", StringComparison.OrdinalIgnoreCase) || isImageFolder) &&
                !ImageB64Name.IsMatch(relative))
                problems.Add($"{relative}: base64 data is allowed only as images/<name>.jpg.b64, and images/ holds nothing else");
        }
        return problems;
    }

    // The PC name and user profile of the machine running the test, read at run time. Nothing here
    // is stored in the repository.
    internal static IReadOnlyList<string> RuntimeEnvironmentNeedles()
    {
        var needles = new List<string>();
        void AddName(string? value)
        {
            if (!string.IsNullOrWhiteSpace(value) && value.Trim().Length >= 4) needles.Add(value.Trim());
        }
        void AddPath(string? value)
        {
            if (string.IsNullOrWhiteSpace(value)) return;
            var trimmed = value.Trim().TrimEnd('\\', '/');
            if (trimmed.Length < 4) return;
            needles.Add(trimmed);
            needles.Add(trimmed.Replace('\\', '/'));
            needles.Add(trimmed.Replace("\\", "\\\\", StringComparison.Ordinal));
        }
        AddName(Environment.MachineName);
        AddName(Environment.UserDomainName);
        if (Environment.UserName.Length >= 5) AddName(Environment.UserName);
        AddPath(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
        AddPath(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData));
        AddPath(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData));
        return needles.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    internal static bool IsSyntheticHex(string lowerCaseValue, IReadOnlySet<string> allowedSha256) =>
        lowerCaseValue.Length switch
        {
            32 => SyntheticId.IsMatch(lowerCaseValue),
            64 => SyntheticId.IsMatch(lowerCaseValue) || allowedSha256.Contains(lowerCaseValue),
            _ => false,
        };

    // name is the path relative to the fixture root, with '/' separators.
    internal static IReadOnlyList<string> Scan(
        string name,
        string text,
        IReadOnlySet<string> allowedSha256,
        bool isDocumentation,
        IReadOnlyList<string>? environmentNeedles = null)
    {
        environmentNeedles ??= RuntimeEnvironmentNeedles();
        return name.EndsWith(".b64", StringComparison.OrdinalIgnoreCase)
            ? ScanBase64Image(name, text, allowedSha256, environmentNeedles)
            : ScanText(name, text, allowedSha256, isDocumentation, environmentNeedles);
    }

    // A .b64 file is never skipped: it must be the base64 of a dummy JPEG that passes the
    // structure check, and the decoded bytes go through the same text rules as any data file.
    private static List<string> ScanBase64Image(
        string name,
        string text,
        IReadOnlySet<string> allowedSha256,
        IReadOnlyList<string> environmentNeedles)
    {
        var problems = new List<string>();
        if (!ImageB64Name.IsMatch(name))
            problems.Add($"{name}: base64 data is allowed only as images/<name>.jpg.b64");

        byte[] bytes;
        try
        {
            var compact = Regex.Replace(text, @"\s+", string.Empty);
            if (compact.Length == 0 || !Regex.IsMatch(compact, "^[A-Za-z0-9+/]+={0,2}$"))
            {
                problems.Add($"{name}: content is not plain base64");
                return problems;
            }
            bytes = Convert.FromBase64String(compact);
        }
        catch (FormatException)
        {
            problems.Add($"{name}: content is not valid base64");
            return problems;
        }

        problems.AddRange(HardwareReplayJpegContracts.Inspect(name, bytes));
        // The table bytes of a JPEG are arbitrary, so the digit-run rules would fire on them; they
        // are applied to the image data, the only place left in a dummy image that is free-form.
        problems.AddRange(ScanText(
            name + " (decoded)", PrintableAscii(bytes), allowedSha256, isDocumentation: false,
            environmentNeedles, isBinary: true));
        var imageData = HardwareReplayJpegContracts.ImageData(bytes);
        if (imageData is not null)
        {
            foreach (Match match in LongDigitRun.Matches(PrintableAscii(imageData)))
                problems.Add($"{name} (decoded): {match.Length}-digit number in the image data");
        }
        return problems;
    }

    // Bytes that are not printable ASCII become spaces, so that binary bytes next to a word do not
    // hide it from word-boundary rules (Latin-1 letters count as word characters).
    private static string PrintableAscii(byte[] bytes) =>
        string.Create(bytes.Length, bytes, static (span, source) =>
        {
            for (var i = 0; i < source.Length; i++)
                span[i] = source[i] is >= 0x20 and <= 0x7E ? (char)source[i] : ' ';
        });

    private static List<string> ScanText(
        string name,
        string text,
        IReadOnlySet<string> allowedSha256,
        bool isDocumentation,
        IReadOnlyList<string> environmentNeedles,
        bool isBinary = false)
    {
        var problems = new List<string>();
        text = ExpandTerminalResultHex(name, text, problems);

        foreach (var needle in environmentNeedles)
        {
            if (text.Contains(needle, StringComparison.OrdinalIgnoreCase))
            {
                problems.Add($"{name}: contains the PC name or user profile of the machine running this test");
                break;
            }
        }

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

        // Values are never echoed back: a failing run must not copy a real identifier into a log.
        foreach (Match match in HexRun.Matches(text))
        {
            if (!IsSyntheticHex(match.Value.ToLowerInvariant(), allowedSha256))
                problems.Add($"{name}: hexadecimal run of {match.Length} characters at offset {match.Index} is not a synthetic value");
        }
        foreach (Match match in DashedGuid.Matches(text))
        {
            if (!IsSyntheticHex(match.Value.Replace("-", string.Empty, StringComparison.Ordinal).ToLowerInvariant(), allowedSha256))
                problems.Add($"{name}: dashed GUID at offset {match.Index} is not a synthetic value");
        }
        foreach (Match match in EmailAddress.Matches(text))
            problems.Add($"{name}: e-mail address at offset {match.Index}");

        // Allowed hex values and GUIDs can contain digit runs that look like dates or epochs; they
        // were judged as a whole above.
        var numeric = HexRun.Replace(text, match => new string(' ', match.Length));
        numeric = DashedGuid.Replace(numeric, match => new string(' ', match.Length));

        foreach (Match match in Epoch.Matches(numeric))
            problems.Add($"{name}: {match.Length}-digit epoch timestamp at offset {match.Index}");

        foreach (Match match in DateNumeric.Matches(numeric))
        {
            if (match.Groups[1].Value != "2026" || match.Groups[3].Value != "01")
                problems.Add($"{name}: date at offset {match.Index} is outside the shifted fixture month");
        }
        foreach (Match match in DateJapanese.Matches(numeric))
        {
            if (match.Groups[1].Value != "2026" || int.Parse(match.Groups[2].Value) != 1)
                problems.Add($"{name}: date at offset {match.Index} is outside the shifted fixture month");
        }

        if (!isDocumentation && !isBinary)
        {
            foreach (Match match in OpaqueTokenValue.Matches(numeric))
            {
                var token = match.Groups[1].Value;
                if (token.Any(char.IsDigit) && !AllowedTokenValues.Contains(token))
                    problems.Add($"{name}: opaque identifier-like value of {token.Length} characters at offset {match.Index}");
            }
            foreach (Match match in LongDigitRun.Matches(numeric))
                problems.Add($"{name}: {match.Length}-digit number at offset {match.Index} (serial or counter shape)");
        }
        return problems;
    }

    // The pair journal keeps the Agent's terminal result as one long hex string. Scan the text it
    // encodes, not the opaque hex.
    private static string ExpandTerminalResultHex(string name, string text, List<string> problems) =>
        Regex.Replace(
            text,
            "\"terminalResultHex\"\\s*:\\s*\"([0-9a-fA-F]+)\"",
            match =>
            {
                try
                {
                    return "\"terminalResultDecoded\":" + Encoding.UTF8.GetString(Convert.FromHexString(match.Groups[1].Value));
                }
                catch (FormatException)
                {
                    problems.Add($"{name}: terminalResultHex at offset {match.Index} is not valid hexadecimal");
                    return string.Empty;
                }
            });
}

internal static class HardwareReplayJpegContracts
{
    // The only COM text a dummy image may carry. An explicit list: adding a fixture image means
    // adding its label here.
    private static readonly string[] AllowedNotices =
    [
        "A0 replay fixture: synthetic image, not a photograph (single CAM-A)",
        "A0 replay fixture: synthetic image, not a photograph (dual CAM-A)",
        "A0 replay fixture: synthetic image, not a photograph (dual CAM-B)",
    ];

    // marker -> allowed value of the segment's own length field (the two length bytes included).
    // COM (FE) has its own exact-text check. Fixed sizes leave no room for a payload.
    private static readonly Dictionary<byte, int[]> AllowedSegmentLengths = new()
    {
        [0xE0] = [16],
        [0xDB] = [67],
        [0xC0] = [17],
        [0xC4] = [31, 181],
        [0xDA] = [12],
    };

    internal const int MaxScanDataBytes = 32;

    // The bytes between the scan header and EOI, or null when the image has no scan header.
    internal static byte[]? ImageData(byte[] bytes)
    {
        var limit = bytes.Length - 2;
        for (var index = 2; index + 4 <= limit;)
        {
            if (bytes[index] != 0xFF) return null;
            var length = (bytes[index + 2] << 8) | bytes[index + 3];
            if (length < 2 || index + 2 + length > limit) return null;
            if (bytes[index + 1] == 0xDA) return bytes[(index + 2 + length)..limit];
            index += 2 + length;
        }
        return null;
    }
    internal const int MaxFileBytes = 4096;

    internal static void RequireDummyJpeg(string name, byte[] bytes)
    {
        var problems = Inspect(name, bytes);
        Check.True(problems.Count == 0, string.Join("; ", problems));
    }

    // Walks the segments of a dummy JPEG and returns every way it differs from the dummy: only
    // JFIF, the one notice, tables, the frame header and the scan header may appear (an
    // allow-list, so EXIF/XMP/ICC/any APPn or unknown marker fails), segment sizes are fixed, and
    // the entropy-coded data is a few bytes at most.
    internal static IReadOnlyList<string> Inspect(string name, byte[] bytes)
    {
        var problems = new List<string>();
        if (bytes.Length is <= 100 or > MaxFileBytes)
        {
            problems.Add($"{name}: a replay dummy JPEG must be a few hundred bytes, got {bytes.Length}.");
            return problems;
        }
        if (!(bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[^2] == 0xFF && bytes[^1] == 0xD9))
        {
            problems.Add($"{name}: dummy JPEG must have SOI and EOI markers.");
            return problems;
        }

        var counts = new Dictionary<byte, int>();
        var limit = bytes.Length - 2; // EOI excluded
        var index = 2;
        var reachedScan = false;
        while (index + 4 <= limit)
        {
            if (bytes[index] != 0xFF)
            {
                problems.Add($"{name}: JPEG marker structure is invalid at offset {index}.");
                return problems;
            }
            var marker = bytes[index + 1];
            var length = (bytes[index + 2] << 8) | bytes[index + 3];
            counts[marker] = counts.GetValueOrDefault(marker) + 1;
            if (marker != 0xFE && !AllowedSegmentLengths.ContainsKey(marker))
            {
                problems.Add($"{name}: JPEG segment 0x{marker:X2} is not allowed in a dummy image (allowed: E0, FE, DB, C0, C4, DA).");
                return problems;
            }
            if (length < 2 || index + 2 + length > limit)
            {
                problems.Add($"{name}: JPEG segment 0x{marker:X2} has an invalid length.");
                return problems;
            }
            if (marker != 0xFE && !AllowedSegmentLengths[marker].Contains(length))
            {
                problems.Add($"{name}: JPEG segment 0x{marker:X2} has length {length}, not the fixed size of the dummy.");
                return problems;
            }

            switch (marker)
            {
                case 0xE0:
                    if (Encoding.ASCII.GetString(bytes, index + 4, 5) != "JFIF\0")
                        problems.Add($"{name}: the APP0 segment must be the plain JFIF header.");
                    break;
                case 0xFE:
                    var comment = Encoding.Latin1.GetString(bytes, index + 4, length - 2);
                    if (!AllowedNotices.Contains(comment, StringComparer.Ordinal))
                        problems.Add($"{name}: the COM segment must be exactly the synthetic-image notice.");
                    break;
                case 0xC0:
                    var height = (bytes[index + 5] << 8) | bytes[index + 6];
                    var width = (bytes[index + 7] << 8) | bytes[index + 8];
                    if (!(width == 7360 && height == 4912))
                        problems.Add($"{name}: dummy JPEG must declare the D810 L size 7360x4912, got {width}x{height}.");
                    break;
            }

            if (marker == 0xDA)
            {
                reachedScan = true;
                var scanBytes = limit - (index + 2 + length);
                if (scanBytes > MaxScanDataBytes)
                    problems.Add($"{name}: the image data is {scanBytes} bytes; a dummy image keeps at most {MaxScanDataBytes}.");
                break;
            }
            index += 2 + length;
        }

        if (!reachedScan) problems.Add($"{name}: dummy JPEG has no scan header.");
        foreach (var (marker, min, max) in new (byte, int, int)[]
                 { (0xE0, 1, 1), (0xFE, 1, 1), (0xDB, 1, 4), (0xC0, 1, 1), (0xC4, 1, 4), (0xDA, 1, 1) })
        {
            var count = counts.GetValueOrDefault(marker);
            if (count < min || count > max)
                problems.Add($"{name}: dummy JPEG has {count} segment(s) 0x{marker:X2}, expected {min} to {max}.");
        }
        return problems;
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
