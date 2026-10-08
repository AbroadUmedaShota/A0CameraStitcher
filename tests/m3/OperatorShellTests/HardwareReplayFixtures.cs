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

    // The fixture folder must have been copied next to the test binary. The failure text has no path:
    // the full path holds the profile of the machine running the test.
    internal static void RequireDirectory(string root)
    {
        if (!System.IO.Directory.Exists(root))
            throw new InvalidOperationException("The replay fixtures were not copied next to the test binary.");
    }

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

    // A Windows SID (S-1-5-21-a-b-c, also the shorter well-known forms with 3 or more parts) and a
    // UNC path (\\server\share, //server/share; also with doubled backslashes inside JSON text).
    private static readonly Regex WindowsSid = new(@"(?<![A-Za-z0-9\-])S-1-\d{1,3}(?:-\d+){2,}(?![A-Za-z0-9\-])", RegexOptions.Compiled);
    private static readonly Regex UncPath = new(
        @"(?<![A-Za-z0-9:\\/.])(?:\\{2,4}|//)[A-Za-z0-9_$][A-Za-z0-9._$\-]*(?:\\{1,2}|/)", RegexOptions.Compiled);

    // The word rules that are also applied to the names of the files (#260): a host name, a PC name
    // or a USB / PnP ID in a path is published as it is in a content.
    private const string UsbOrPnpIdentifierName = "USB or PnP identifier";
    private const string AccountOrHostNameName = "windows account or host name";
    private const string DefaultHostNameName = "default windows host name";
    private const string DefaultHostNameOtherCaseName = "default windows host name (other case)";
    private static readonly Regex UsbOrPnpIdentifier = new(
        @"\b(vid|pid)_[0-9a-f]{4}|usb[\\#]|\\\\\?\\|device ?id|instance ?id|pnp", RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private static readonly Regex AccountOrHostName = new(
        @"\b[A-Z]{2,6}-\d{2}-NOTE\b|\buser name\b", RegexOptions.IgnoreCase | RegexOptions.Compiled);
    // Windows' own default names (DESKTOP-xxxxxxx and the like). The upper-case rule is
    // case-sensitive on purpose: "win-x64" style runtime identifiers are not host names. A
    // lower-case copy of the PC name is still the PC name, so DESKTOP- and LAPTOP- are also
    // read in any case, and WIN- in any case when it has the 11 characters Windows generates.
    private static readonly Regex DefaultHostName = new(@"\b(?:DESKTOP|LAPTOP|WIN)-[A-Z0-9]{5,}\b", RegexOptions.Compiled);
    private static readonly Regex DefaultHostNameOtherCase = new(
        @"\b(?:desktop|laptop)-[a-z0-9]{5,}\b|\bwin-[a-z0-9]{11}\b", RegexOptions.IgnoreCase | RegexOptions.Compiled);

    // Forbidden anywhere in a data file. Documentation (README.md) is exempt from the word rules
    // only, because it has to say which categories were removed.
    private static readonly (string Name, Regex Pattern)[] DataFileRules =
    [
        ("user profile path", new Regex(@"[\\/]users[\\/]", RegexOptions.IgnoreCase)),
        ("app data folder", new Regex("appdata", RegexOptions.IgnoreCase)),
        ("serial number", new Regex("serial", RegexOptions.IgnoreCase)),
        (UsbOrPnpIdentifierName, UsbOrPnpIdentifier),
        (AccountOrHostNameName, AccountOrHostName),
        (DefaultHostNameName, DefaultHostName),
        (DefaultHostNameOtherCaseName, DefaultHostNameOtherCase),
        ("host, machine, account or owner field", new Regex(
            "\"[\\w\\-]*(?:computer|machine|host|pc|user|account|login)[\\w\\-]*name[\\w\\-]*\"\\s*:|\"[\\w\\-]*owner[\\w\\-]*\"\\s*:",
            RegexOptions.IgnoreCase)),
        // Keys that name a person or machine without the word "name" in them.
        ("artist, author, host or account field", new Regex(
            "\"(?:[\\w\\-]*(?:artist|author|copyright|creator)[\\w\\-]*|host|pc|user|computer|machine|account|login)\"\\s*:",
            RegexOptions.IgnoreCase)),
        ("Windows SID", WindowsSid),
        ("UNC path (server name)", UncPath),
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

    // A run of 6 or more digits (7-digit body serials, long counters), also when a letter or a
    // hyphen touches it (CAM-A-1234567, SN1234567). Only a digit next to it (a longer number), or a
    // dot with a digit on its other side (a decimal fraction), keeps it out. A dot alone does not:
    // 3012345.jpg is a number with a file extension, 1234567.5 is a fraction (#247). The
    // synthetic shapes below are removed first.
    // The one exception to the dot rule (#260): a run behind "digit." and in front of ".letter" is a
    // name with an extension (9.1234567.jpg), not a fraction.
    private static readonly Regex LongDigitRun = new(
        @"(?<![0-9])(?:(?<![0-9]\.)\d{6,}(?![0-9])(?!\.[0-9])|(?<=[0-9]\.)\d{6,}(?![0-9])(?=\.[A-Za-z]))", RegexOptions.Compiled);

    private static readonly Regex PathDigitRun = new(@"(?<![0-9])\d{6,}(?![0-9])", RegexOptions.Compiled);

    // The synthetic run IDs of the fixtures, accepted as a whole shape: run-231###-#,
    // hybrid-tx-231###-# and dual-leg-CAM-[AB]-run-231###-#. A letter, digit or hyphen in front, or
    // a digit, letter or hyphen behind, makes it a different string.
    private static readonly Regex SyntheticRunId = new(
        @"(?<![A-Za-z0-9\-])(?:dual-leg-CAM-[AB]-run-|hybrid-tx-|run-)231\d{3}-\d(?![0-9A-Za-z\-])", RegexOptions.Compiled);

    // sdkVersion values the real Agent wrote (a version number with 7 digits is no identifier).
    // An explicit list of whole values.
    private static readonly Regex SyntheticSdkVersion = new(
        "(\"sdkVersion\"\\s*:\\s*\")(?:0x3010000\\+windows-wpd-1;access=read-write;qos=impersonation;command-target=functional|Nikon-D810-licensed-dual-session)(\")",
        RegexOptions.Compiled);

    // Dates the first scans did not read: M/d/yyyy and d.M.yyyy (day and month either way round),
    // yyyy-M-d with one-digit parts, "15 Jun 2026" / "Jun 15, 2026" (RFC 1123 and prose) and
    // yyyyMMddHHmmss (DSC_20260615050532), and the year first with a month name (2026-Jun-15, #260).
    // Only January 2026 is allowed, as for the other forms.
    private static readonly Regex DateYearFirstLoose = new(
        @"(?<!\d)((?:19|20)\d{2})([-/:.])(\d{1,2})\2(\d{1,2})(?!\d)", RegexOptions.Compiled);
    private static readonly Regex DateYearLast = new(
        @"(?<![\d.])(\d{1,2})([/.\-])(\d{1,2})\2((?:19|20)\d{2})(?!\d)", RegexOptions.Compiled);
    private const string MonthNames = "jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec";
    // The parts are separated by white space, a hyphen, a slash or a dot (15 Jun 2026, 15-Jun-2026,
    // 15/Jun/2026, Jun.15.2026).
    private const string MonthNameSeparator = @"[\s\-/.]+";
    private static readonly Regex DateMonthName = new(
        @"(?<![A-Za-z0-9])(?:(\d{1,2})(?:st|nd|rd|th)?" + MonthNameSeparator + "(" + MonthNames + @")[a-z]*\.?,?" + MonthNameSeparator +
        @"((?:19|20)\d{2})|(" + MonthNames + @")[a-z]*\.?" + MonthNameSeparator + @"(\d{1,2})(?:st|nd|rd|th)?,?" + MonthNameSeparator +
        @"((?:19|20)\d{2}))(?![A-Za-z0-9])",
        RegexOptions.Compiled | RegexOptions.IgnoreCase);
    // The year first (2026-Jun-15, #260) is a separate rule, scanned on its own: Regex.Matches
    // returns no overlapping matches, so as an alternative of DateMonthName its allowed form
    // "2026 Jan 15" would consume the start of "2026 Jan 15 2025" and hide the date behind it.
    private static readonly Regex DateMonthNameYearFirst = new(
        @"(?<![A-Za-z0-9])((?:19|20)\d{2})" + MonthNameSeparator + "(" + MonthNames + @")[a-z]*\.?" + MonthNameSeparator +
        @"(\d{1,2})(?:st|nd|rd|th)?(?![A-Za-z0-9])",
        RegexOptions.Compiled | RegexOptions.IgnoreCase);
    private static readonly Regex DateTimeCompact = new(
        @"(?<!\d)((?:19|20)\d{2})(0[1-9]|1[0-2])(0[1-9]|[12]\d|3[01])(?:[01]\d|2[0-3])[0-5]\d[0-5]\d(?:\d{3})?(?!\d)", RegexOptions.Compiled);

    // A string value of this many characters or more that looks like base64, and whatever sits
    // under a key that ends in Base64, must decode to a dummy JPEG (an image smuggled into a JSON
    // string, such as an Agent response's frame, is otherwise invisible to the text rules).
    internal const int EmbeddedBase64MinLength = 64;
    private static readonly Regex JsonStringLiteral = new("\"((?:[^\"\\\\]|\\\\.)*)\"", RegexOptions.Compiled | RegexOptions.Singleline);
    private static readonly Regex Base64KeyedValue = new(
        "\"[A-Za-z0-9_]*Base64\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"", RegexOptions.Compiled | RegexOptions.IgnoreCase | RegexOptions.Singleline);
    private static readonly Regex Base64KeyedNonString = new(
        "\"[A-Za-z0-9_]*Base64\"\\s*:\\s*(?![\"\\s]|null\\b)", RegexOptions.Compiled | RegexOptions.IgnoreCase);

    // The synthetic 32/64-digit values the fixtures use: "f231", four free digits, a run of zeros,
    // two to four free digits. The whole shape is checked, not the prefix, so a real-looking value
    // that merely begins with "f231" is rejected.
    private static readonly Regex SyntheticId = new("^f231[0-9a-f]{4}0+[0-9a-f]{2,4}$", RegexOptions.Compiled);

    // The file names are published too (#243): the relative path of every file goes through the
    // same shape rules as the content (hex runs, GUIDs, e-mail, epoch, dates, digit runs) and the
    // PC name and profile of the machine running the test.
    internal static IReadOnlyList<string> CheckFileSet(string root, IReadOnlyList<string>? environmentNeedles = null)
    {
        environmentNeedles ??= RuntimeEnvironmentNeedles();
        var problems = new List<string>();
        foreach (var path in System.IO.Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(root, path).Replace('\\', '/');
            // The messages name the file, and the name may hold the value that is wrong (#247).
            var shown = HideValues(relative, environmentNeedles);
            problems.AddRange(ScanRelativePath(relative, environmentNeedles));
            var extension = Path.GetExtension(path);
            if (!AllowedExtensions.Contains(extension, StringComparer.OrdinalIgnoreCase))
                problems.Add($"{shown}: file type is not allowed in the replay fixtures (images are stored only as .jpg.b64 dummies)");
            if (Path.GetFileName(path).EndsWith(".jpg", StringComparison.OrdinalIgnoreCase) ||
                Path.GetFileName(path).EndsWith(".jpeg", StringComparison.OrdinalIgnoreCase))
                problems.Add($"{shown}: a real image file must never be stored here");
            var isImageFolder = relative.StartsWith("images/", StringComparison.OrdinalIgnoreCase);
            if ((extension.Equals(".b64", StringComparison.OrdinalIgnoreCase) || isImageFolder) &&
                !ImageB64Name.IsMatch(relative))
                problems.Add($"{shown}: base64 data is allowed only as images/<name>.jpg.b64, and images/ holds nothing else");
        }
        return problems;
    }

    // The relative path with every part that one of the path rules matches (hex run, GUID, e-mail,
    // epoch, date, digit run, PC name or profile of this machine) replaced by "***", for the
    // messages: a file name that fails a rule must not be copied into a log (#247).
    internal static string HideValues(string relative, IReadOnlyList<string> environmentNeedles)
    {
        var hidden = new bool[relative.Length];
        void Mark(int index, int length)
        {
            for (var i = index; i < index + length && i < hidden.Length; i++) hidden[i] = true;
        }
        Regex[] patterns =
        [
            HexRun, DashedGuid, EmailAddress, Epoch, DateNumeric, DateJapanese, DateYearFirstLoose, DateYearLast,
            DateMonthName, DateMonthNameYearFirst, DateTimeCompact, PathDigitRun,
            UsbOrPnpIdentifier, AccountOrHostName, DefaultHostName, DefaultHostNameOtherCase,
        ];
        foreach (var pattern in patterns)
        {
            foreach (Match match in pattern.Matches(relative)) Mark(match.Index, match.Length);
        }
        foreach (var needle in environmentNeedles)
        {
            if (needle.Length == 0) continue;
            for (var at = relative.IndexOf(needle, StringComparison.OrdinalIgnoreCase); at >= 0;
                 at = relative.IndexOf(needle, at + 1, StringComparison.OrdinalIgnoreCase))
                Mark(at, needle.Length);
        }
        var builder = new StringBuilder();
        for (var i = 0; i < relative.Length; i++)
        {
            if (!hidden[i]) builder.Append(relative[i]);
            else if (i == 0 || !hidden[i - 1]) builder.Append("***");
        }
        return builder.ToString();
    }

    // Directory names count as well, so every part of the relative path is covered by the text.
    internal static IReadOnlyList<string> ScanRelativePath(string relative, IReadOnlyList<string> environmentNeedles)
    {
        var label = HideValues(relative, environmentNeedles) + " (file name)";
        var problems = new List<string>();
        problems.AddRange(ScanEnvironment(label, relative, environmentNeedles));
        problems.AddRange(ScanShapes(label, relative, new HashSet<string>(StringComparer.Ordinal), checkLooseValues: false));
        // A host name, a PC name of the XX-99-NOTE form and a USB / PnP ID are as much an identifier
        // in a file name as in a content (#260).
        foreach (var (ruleName, pattern) in DataFileRules)
        {
            if (ruleName is UsbOrPnpIdentifierName or AccountOrHostNameName or DefaultHostNameName or DefaultHostNameOtherCaseName &&
                pattern.IsMatch(relative))
                problems.Add($"{label}: contains a {ruleName}");
        }
        // A dot in a name is the extension, not a decimal point (cam1234567.json), so the digit-run
        // rule here only looks at digits touching digits.
        // Hex runs and GUIDs were judged as a whole above, like the synthetic run IDs.
        string Blank(Match match) => new(' ', match.Length);
        var withoutSyntheticIds = SyntheticRunId.Replace(DashedGuid.Replace(HexRun.Replace(relative, Blank), Blank), Blank);
        foreach (Match match in PathDigitRun.Matches(withoutSyntheticIds))
            problems.Add($"{label}: {match.Length}-digit number at offset {match.Index} (serial or counter shape)");
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
        var problems = name.EndsWith(".b64", StringComparison.OrdinalIgnoreCase)
            ? ScanBase64Image(name, text, allowedSha256, environmentNeedles)
            : ScanText(name, text, allowedSha256, isDocumentation, environmentNeedles);
        // Every message starts with the file name, which may hold the value that is wrong (#247).
        var shown = HideValues(name, environmentNeedles);
        return shown == name
            ? problems
            : problems.Select(problem => problem.Replace(name, shown, StringComparison.Ordinal)).ToList();
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

        problems.AddRange(ScanDummyImageBytes(name, bytes, allowedSha256, environmentNeedles));
        return problems;
    }

    // The structure check of a dummy JPEG plus the text rules on what it decodes to. Used for the
    // .b64 files and for base64 images found inside JSON strings.
    private static List<string> ScanDummyImageBytes(
        string name,
        byte[] bytes,
        IReadOnlySet<string> allowedSha256,
        IReadOnlyList<string> environmentNeedles)
    {
        var problems = new List<string>();
        problems.AddRange(HardwareReplayJpegContracts.Inspect(name, bytes));
        if (!HardwareReplayJpegContracts.IsPinned(bytes))
            problems.Add($"{name}: the dummy image is not one of the three pinned images (SHA-256 differs)");
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

        // Images inside JSON strings are judged first; the ones that pass are blanked, so the
        // random-looking base64 text of a dummy image is not read by the rules below.
        if (!isDocumentation && !isBinary)
            problems.AddRange(ScanEmbeddedBase64(name, ref text, allowedSha256, environmentNeedles));

        problems.AddRange(ScanEnvironment(name, text, environmentNeedles));

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

        problems.AddRange(ScanShapes(name, text, allowedSha256, checkLooseValues: !isDocumentation && !isBinary));
        return problems;
    }

    private static List<string> ScanEnvironment(string name, string text, IReadOnlyList<string> environmentNeedles)
    {
        var problems = new List<string>();
        foreach (var needle in environmentNeedles)
        {
            if (text.Contains(needle, StringComparison.OrdinalIgnoreCase))
            {
                problems.Add($"{name}: contains the PC name or user profile of the machine running this test");
                break;
            }
        }
        return problems;
    }

    // The shape rules that apply to any text, a file's content or a file's relative path: hex
    // runs, GUIDs, e-mail addresses, epoch times, dates and (checkLooseValues) opaque tokens and
    // digit runs. Values are never echoed back: a failing run must not copy a real identifier into
    // a log.
    private static List<string> ScanShapes(
        string name,
        string text,
        IReadOnlySet<string> allowedSha256,
        bool checkLooseValues)
    {
        var problems = new List<string>();
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
        // were judged as a whole above. The synthetic run IDs and sdkVersion values are allowed as
        // whole shapes, so their digits are not judged one by one.
        string Blank(Match match) => new(' ', match.Length);
        var numeric = HexRun.Replace(text, Blank);
        numeric = DashedGuid.Replace(numeric, Blank);
        numeric = SyntheticRunId.Replace(numeric, Blank);
        numeric = SyntheticSdkVersion.Replace(numeric, Blank);

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
        foreach (Match match in DateYearFirstLoose.Matches(numeric))
        {
            // Two-digit parts are judged by DateNumeric above; this is for 2026-6-15 and the like.
            if (match.Groups[3].Length != 1 && match.Groups[4].Length != 1) continue;
            if (match.Groups[1].Value != "2026" || int.Parse(match.Groups[3].Value) != 1)
                problems.Add($"{name}: date at offset {match.Index} is outside the shifted fixture month");
        }
        foreach (Match match in DateYearLast.Matches(numeric))
        {
            // Month first or day first: January 2026 is the only month both readings can accept.
            var first = int.Parse(match.Groups[1].Value);
            var second = int.Parse(match.Groups[3].Value);
            var january = (first == 1 && second is >= 1 and <= 31) || (second == 1 && first is >= 1 and <= 31);
            if (match.Groups[4].Value != "2026" || !january)
                problems.Add($"{name}: date at offset {match.Index} is outside the shifted fixture month");
        }
        foreach (Match match in DateMonthName.Matches(numeric))
        {
            var month = match.Groups[2].Success ? match.Groups[2].Value : match.Groups[4].Value;
            var year = match.Groups[3].Success ? match.Groups[3].Value : match.Groups[6].Value;
            if (year != "2026" || !month.StartsWith("jan", StringComparison.OrdinalIgnoreCase))
                problems.Add($"{name}: date at offset {match.Index} is outside the shifted fixture month");
        }
        // The year first (2026-Jun-15) is scanned apart from the two forms above (#260).
        foreach (Match match in DateMonthNameYearFirst.Matches(numeric))
        {
            if (match.Groups[1].Value != "2026" || !match.Groups[2].Value.StartsWith("jan", StringComparison.OrdinalIgnoreCase))
                problems.Add($"{name}: date at offset {match.Index} is outside the shifted fixture month");
        }
        foreach (Match match in DateTimeCompact.Matches(numeric))
        {
            if (match.Groups[1].Value != "2026" || match.Groups[2].Value != "01")
                problems.Add($"{name}: date and time at offset {match.Index} is outside the shifted fixture month");
        }

        if (checkLooseValues)
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

    private static readonly Regex DataUriBase64 = new(
        @"^data:[A-Za-z0-9.+/\-]*(?:;[A-Za-z0-9=.+\-]+)*;base64,(.*)$", RegexOptions.Compiled | RegexOptions.Singleline | RegexOptions.IgnoreCase);

    private static readonly Regex Base64Alphabet = new("^[A-Za-z0-9+/_\\-]+={0,2}$", RegexOptions.Compiled);
    private static readonly Regex HexOnly = new("^[0-9a-fA-F]+$", RegexOptions.Compiled);

    // Base64 images inside JSON strings (an Agent response's frameJpegBase64 / frameBase64).
    // Everything under a key ending in Base64, and every string value of 64 characters or more
    // that looks like base64 (and is not plain hex, which the hex rules judge), must decode to a
    // dummy JPEG that passes the structure check and the text rules. Anything else is rejected,
    // including text that cannot be decoded: a value that cannot be shown harmless is not allowed.
    private static List<string> ScanEmbeddedBase64(
        string name,
        ref string text,
        IReadOnlySet<string> allowedSha256,
        IReadOnlyList<string> environmentNeedles)
    {
        var problems = new List<string>();
        var handled = new HashSet<int>();
        var accepted = new List<(int Index, int Length)>();

        foreach (Match match in Base64KeyedNonString.Matches(text))
            problems.Add($"{name}: the value under a *Base64 key at offset {match.Index} must be a string");

        foreach (Match match in Base64KeyedValue.Matches(text))
        {
            var group = match.Groups[1];
            handled.Add(group.Index);
            var value = UnescapeJsonString(group.Value);
            if (value.Length == 0) continue;
            var found = CheckEmbeddedImage(
                name, $"base64 under a *Base64 key at offset {group.Index}", value, allowedSha256, environmentNeedles);
            if (found.Count == 0) accepted.Add((group.Index, group.Length));
            problems.AddRange(found);
        }

        foreach (Match match in JsonStringLiteral.Matches(text))
        {
            var group = match.Groups[1];
            if (group.Length < EmbeddedBase64MinLength || handled.Contains(group.Index)) continue;

            // A string followed by ':' is a key, not a value.
            var next = match.Index + match.Length;
            while (next < text.Length && char.IsWhiteSpace(text[next])) next++;
            if (next < text.Length && text[next] == ':') continue;

            // Only line breaks may sit inside (wrapped base64); a space means prose. A data URI is
            // read by its base64 part (#247).
            var value = UnescapeJsonString(group.Value);
            var dataUri = DataUriBase64.Match(value);
            var compact = Regex.Replace(dataUri.Success ? dataUri.Groups[1].Value : value, @"[\r\n]+", string.Empty);
            if (compact.Length < EmbeddedBase64MinLength || !Base64Alphabet.IsMatch(compact) || HexOnly.IsMatch(compact)) continue;
            var found = CheckEmbeddedImage(
                name, $"base64-like string of {compact.Length} characters at offset {group.Index}", compact, allowedSha256, environmentNeedles);
            if (found.Count == 0) accepted.Add((group.Index, group.Length));
            problems.AddRange(found);
        }

        if (accepted.Count > 0)
        {
            var chars = text.ToCharArray();
            foreach (var (index, length) in accepted) Array.Fill(chars, ' ', index, length);
            text = new string(chars);
        }
        return problems;
    }

    private static List<string> CheckEmbeddedImage(
        string name,
        string description,
        string value,
        IReadOnlySet<string> allowedSha256,
        IReadOnlyList<string> environmentNeedles)
    {
        var label = $"{name} ({description})";
        if (!TryDecodeBase64(value, out var bytes))
            return [$"{label}: not decodable base64, so it cannot be shown to be a dummy image"];
        var problems = ScanDummyImageBytes(label, bytes, allowedSha256, environmentNeedles);
        if (problems.Count > 0) problems.Insert(0, $"{label}: embedded base64 is not a dummy JPEG");
        return problems;
    }

    private static bool TryDecodeBase64(string value, out byte[] bytes)
    {
        bytes = [];
        // Both alphabets (standard and URL-safe), whitespace ignored, padding optional.
        var compact = Regex.Replace(value, @"\s+", string.Empty).Replace('-', '+').Replace('_', '/');
        if (compact.Length == 0 || !Regex.IsMatch(compact, "^[A-Za-z0-9+/]+={0,2}$")) return false;
        compact = compact.TrimEnd('=');
        compact = compact.PadRight(compact.Length + ((4 - (compact.Length % 4)) % 4), '=');
        try
        {
            bytes = Convert.FromBase64String(compact);
            return true;
        }
        catch (FormatException)
        {
            return false;
        }
    }

    // The JSON string with its escapes (\/, \uXXXX, ...) resolved; the raw text when it is not a
    // valid JSON string body.
    private static string UnescapeJsonString(string raw)
    {
        try
        {
            return JsonSerializer.Deserialize<string>("\"" + raw + "\"") ?? raw;
        }
        catch (JsonException)
        {
            return raw;
        }
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

    // marker -> the payloads (the bytes after the two length bytes, in hex) a dummy image may carry.
    // The JFIF header, the frame header, the scan header and the quantization and Huffman tables
    // are the same in every dummy, so each is compared byte for byte: a table has 64 or more free
    // bytes that could otherwise hold a value (#243). COM (FE) has its own exact-text check.
    // The Huffman tables are the standard ones (JPEG Annex K.3); the quantization tables are the
    // two the dummy generator wrote.
    private static readonly Dictionary<byte, string[]> AllowedSegmentPayloads = new()
    {
        [0xE0] = ["4A46494600010101006000600000"],
        [0xDB] =
        [
            "000302020302020303030304030304050805050404050A070706080C0A0C0C0B0A0B0B0D0E12100D0E110E0B0B1016101113141515150C0F171816141812141514",
            "0103040405040509050509140D0B0D1414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414141414",
        ],
        [0xC0] = ["0813301CC003012200021101031101"],
        [0xC4] =
        [
            "0000010501010101010100000000000000000102030405060708090A0B",
            "100002010303020403050504040000017D01020300041105122131410613516107227114328191A1082342B1C11552D1F02433627282090A161718191A25262728292A3435363738393A434445464748494A535455565758595A636465666768696A737475767778797A838485868788898A92939495969798999AA2A3A4A5A6A7A8A9AAB2B3B4B5B6B7B8B9BAC2C3C4C5C6C7C8C9CAD2D3D4D5D6D7D8D9DAE1E2E3E4E5E6E7E8E9EAF1F2F3F4F5F6F7F8F9FA",
            "0100030101010101010101010000000000000102030405060708090A0B",
            "1100020102040403040705040400010277000102031104052131061241510761711322328108144291A1B1C109233352F0156272D10A162434E125F11718191A262728292A35363738393A434445464748494A535455565758595A636465666768696A737475767778797A82838485868788898A92939495969798999AA2A3A4A5A6A7A8A9AAB2B3B4B5B6B7B8B9BAC2C3C4C5C6C7C8C9CAD2D3D4D5D6D7D8D9DAE2E3E4E5E6E7E8E9EAF2F3F4F5F6F7F8F9FA",
        ],
        [0xDA] = ["03010002110311003F00"],
    };
    internal const int MaxScanDataBytes = 32;

    // The three dummy images by their SHA-256 (#247). The structure check leaves up to
    // MaxScanDataBytes of image data free, so a dummy image is accepted only as one of these three
    // byte strings. Changing a dummy image means changing its hash here.
    internal static readonly IReadOnlyDictionary<string, string> PinnedSha256 = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        ["single-cam-a"] = "7be3a2b90fdc9e6d8c7bcc87985aa7893f3192e8562a941f6b9b05c89b3e79c6",
        ["dual-cam-a"] = "febcdb554f3a14f918f2cae41130d254edd38f9c207b16f24e787249a2981b19",
        ["dual-cam-b"] = "7c6ff5ee413bf1eca56ecef055fbfe58e42af688c1c04d60342038af701a0eda",
    };

    internal static bool IsPinned(byte[] bytes) =>
        PinnedSha256.Values.Contains(Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(), StringComparer.Ordinal);

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
        var seenPayloads = new HashSet<string>(StringComparer.Ordinal);
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
            if (marker != 0xFE && !AllowedSegmentPayloads.ContainsKey(marker))
            {
                problems.Add($"{name}: JPEG segment 0x{marker:X2} is not allowed in a dummy image (allowed: E0, FE, DB, C0, C4, DA).");
                return problems;
            }
            if (length < 2 || index + 2 + length > limit)
            {
                problems.Add($"{name}: JPEG segment 0x{marker:X2} has an invalid length.");
                return problems;
            }
            if (marker != 0xFE)
            {
                var payload = Convert.ToHexString(bytes, index + 4, length - 2);
                if (!AllowedSegmentPayloads[marker].Contains(payload, StringComparer.Ordinal))
                {
                    problems.Add($"{name}: JPEG segment 0x{marker:X2} is not byte for byte one of the segments of the dummy image.");
                    return problems;
                }
                if (!seenPayloads.Add(marker.ToString("X2") + payload))
                {
                    problems.Add($"{name}: JPEG segment 0x{marker:X2} is repeated.");
                    return problems;
                }
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
