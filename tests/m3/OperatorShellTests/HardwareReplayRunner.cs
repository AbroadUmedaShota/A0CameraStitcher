using System.Text;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation.DualCamera;
using A0CameraStitcher.M3.Foundation.Hardware;
using A0CameraStitcher.M3.OperatorShell.Hardware;
using A0CameraStitcher.M3.OperatorShell.ViewModels;

// GitHub Issue #231: the contracts that feed the replay fixtures to the app's single-camera and
// dual-camera save paths. The shell-level (OperatorShellViewModel) path is a separate scenario in
// Program.cs because it needs the WPF command scope helpers declared there.
internal static class HardwareReplayContracts
{
    internal static async Task RunAsync()
    {
        FixtureFilesAreAnonymizedAndSelfConsistent();
        AnonymizationRulesRejectKnownBadShapes();
        AnonymizationRulesCloseTheKnownGaps();
        AnonymizationRulesCloseTheGapsOfTheSecondAudit();
        OperatorInputsAndDurableStateLoadThroughProductionReaders();
        await SingleCameraProfileApprovalReproducesTheRealApprovedProfileAsync();
        await SingleCameraReplaySavesThroughViewModelAsync();
        await SingleCameraLegacyLandingIsRejectedByViewModelAsync();
        await DualCameraSucceededReplaySavesThroughWorkflowAndExporterAsync();
        await DualCameraFailedReplayClearsPendingAsync();
        await DualCameraIdentityExpiresAfterFiveMinutesAsync();
    }

    private static IReadOnlyDictionary<string, byte[]> Jpegs() => new Dictionary<string, byte[]>
    {
        ["single-cam-a"] = HardwareReplayFixtures.ReadJpeg("single-cam-a"),
        ["dual-cam-a"] = HardwareReplayFixtures.ReadJpeg("dual-cam-a"),
        ["dual-cam-b"] = HardwareReplayFixtures.ReadJpeg("dual-cam-b"),
    };

    private static void FixtureFilesAreAnonymizedAndSelfConsistent()
    {
        var root = HardwareReplayFixtures.Directory;
        Check.True(System.IO.Directory.Exists(root), $"The replay fixtures were not copied next to the test binary: {root}");

        var fileSetProblems = HardwareReplayAnonymizationRules.CheckFileSet(root);
        Check.True(fileSetProblems.Count == 0, "Replay fixture file set: " + string.Join("; ", fileSetProblems));

        var jpegs = Jpegs();
        foreach (var (name, bytes) in jpegs) HardwareReplayJpegContracts.RequireDummyJpeg(name, bytes);
        var shas = jpegs.ToDictionary(item => item.Key, item => HardwareReplayFixtures.Sha256(item.Value));
        Check.Equal(3, shas.Values.Distinct().Count());
        IReadOnlySet<string> allowedSha = new HashSet<string>(shas.Values, StringComparer.Ordinal);

        var problems = new List<string>();
        var scanned = 0;
        foreach (var path in System.IO.Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(root, path).Replace('\\', '/');
            problems.AddRange(HardwareReplayAnonymizationRules.Scan(
                relative, File.ReadAllText(path), allowedSha, isDocumentation: relative == "README.md"));
            scanned++;
        }
        Check.True(scanned >= 20, $"Too few replay fixture files were scanned ({scanned}).");
        Check.True(problems.Count == 0, "Replay fixtures contain identifier-like values: " + string.Join("; ", problems));

        // The journal's hex-encoded terminal result is the real Agent bytes; the decoded copy kept
        // next to it for review must be exactly those bytes.
        foreach (var scenario in new[] { ReplayDualScenario.Succeeded, ReplayDualScenario.FailedCameraA })
        {
            var folder = HardwareReplayFixtures.ScenarioFolder(scenario);
            var decoded = HardwareReplayFixtures.DualTerminalResult(scenario);
            Check.Equal(
                HardwareReplayFixtures.ReadText($"dual-camera/{folder}/terminal-result.decoded.json").TrimEnd('\n'),
                decoded);
            using var journal = JsonDocument.Parse(HardwareReplayFixtures.ReadText($"dual-camera/{folder}/pair-journal.json"));
            Check.Equal(HardwareReplayFixtures.ScenarioTransactionId(scenario),
                journal.RootElement.GetProperty("transactionId").GetString()!);
        }

        // The sizes and hashes the real Agent recorded must be those of the dummy images that now
        // stand in for the photographs.
        using (var transaction = JsonDocument.Parse(HardwareReplayFixtures.ReadText("single-camera/transaction.json")))
        {
            Check.Equal(shas["single-cam-a"], transaction.RootElement.GetProperty("originalSha256").GetString()!);
            Check.Equal((long)jpegs["single-cam-a"].Length, transaction.RootElement.GetProperty("originalSizeBytes").GetInt64());
            Check.Equal(HardwareReplayFixtures.SingleTransactionId, transaction.RootElement.GetProperty("transactionId").GetString()!);
        }
        RequirePersistedEvent("single-camera/events.jsonl", jpegs["single-cam-a"]);
        RequirePersistedEvent("dual-camera/succeeded/diagnostics-cam-a.events.jsonl", jpegs["dual-cam-a"]);
        RequirePersistedEvent("dual-camera/succeeded/diagnostics-cam-b.events.jsonl", jpegs["dual-cam-b"]);
        using (var response = JsonDocument.Parse(HardwareReplayFixtures.ReadText("single-camera/agent-capture-response.json")))
        {
            var original = response.RootElement.GetProperty("payload").GetProperty("retainedOriginal");
            Check.Equal(shas["single-cam-a"], original.GetProperty("sha256").GetString()!);
            Check.Equal((long)jpegs["single-cam-a"].Length, original.GetProperty("sizeBytes").GetInt64());
        }
    }

    private static void RequirePersistedEvent(string relativePath, byte[] jpeg)
    {
        var persisted = HardwareReplayFixtures.ReadText(relativePath)
            .Split('\n', StringSplitOptions.RemoveEmptyEntries)
            .Select(line => JsonDocument.Parse(line).RootElement)
            .Single(element => element.GetProperty("state").GetString() == "Persisted");
        Check.Equal((long)jpeg.Length, persisted.GetProperty("bytes").GetInt64());
        Check.Equal(HardwareReplayFixtures.Sha256(jpeg), persisted.GetProperty("sha256").GetString()!);
    }

    // The scan is only worth anything if it fires. These inputs are invented stand-ins with the
    // same shapes as the identifiers the real sessions contained; none is a real value.
    private static void AnonymizationRulesRejectKnownBadShapes()
    {
        IReadOnlySet<string> allowed = new HashSet<string>(StringComparer.Ordinal);
        string[] bad =
        [
            "{\"path\":\"C:\\\\Users\\\\someone\\\\x\\\\original.jpg\"}",
            "{\"path\":\"C:/some-other-root/original.jpg\"}",
            "{\"path\":\"D:\\\\a0-other\\\\original.jpg\"}",
            "{\"transactionId\":\"0123456789abcdef0123456789abcdef\"}",
            "{\"sha256\":\"" + new string('a', 64) + "\"}",
            "{\"id\":\"0123456789abcdef0123456789abcdef01234567\"}",
            "{\"note\":\"camera Serial 12345\"}",
            "{\"id\":\"USB\\\\VID_1234&PID_5678\"}",
            "{\"runId\":\"run-1700000000000-1\"}",
            "{\"observedAtUtc\":\"2026-06-15T05:05:32Z\"}",
            "{\"host\":\"ABC-12-NOTE\"}",
        ];
        foreach (var sample in bad)
        {
            Check.True(HardwareReplayAnonymizationRules.Scan("sample.json", sample, allowed, isDocumentation: false).Count > 0,
                $"The anonymization scan missed: {sample}");
        }
        Check.True(HardwareReplayAnonymizationRules.Scan(
                "sample.json",
                "{\"transactionId\":\"" + HardwareReplayFixtures.SingleTransactionId +
                "\",\"path\":\"C:\\\\a0-replay-fixture\\\\x\\\\original.jpg\",\"at\":\"2026-01-01T05:05:32Z\"}",
                allowed, isDocumentation: false).Count == 0,
            "The anonymization scan must accept the synthetic shapes the fixtures use.");

        var hexOnlyIdentifier = Encoding.UTF8.GetBytes("{\"deviceHash\":\"0123456789abcdef0123456789abcdef\"}");
        var journal = "{\"terminalResultHex\":\"" + Convert.ToHexString(hexOnlyIdentifier).ToLowerInvariant() + "\"}";
        Check.True(HardwareReplayAnonymizationRules.Scan("journal.json", journal, allowed, isDocumentation: false).Count > 0,
            "An identifier hidden inside a hex-encoded terminal result must still be found.");

        var folderRoot = HardwareReplayFixtures.NewTestRoot();
        try
        {
            System.IO.Directory.CreateDirectory(Path.Combine(folderRoot, "images"));
            File.WriteAllBytes(Path.Combine(folderRoot, "images", "original.jpg"), [0xFF, 0xD8, 0xFF, 0xD9]);
            Check.True(HardwareReplayAnonymizationRules.CheckFileSet(folderRoot).Count > 0,
                "A real image file in the fixture folder must be rejected.");
        }
        finally
        {
            HardwareReplayFixtures.DeleteTestRoot(folderRoot);
        }
    }

    // GitHub Issue #241: every gap the security audit found in the scan above gets an invented
    // input that must now fail it. None of these is a real value (the real ones are compared by
    // scripts/Test-ReplayFixtureLeak.ps1, never stored). Where the scan must still accept the
    // synthetic shapes the fixtures use, the accepted counterpart is checked too.
    private static void AnonymizationRulesCloseTheKnownGaps()
    {
        IReadOnlySet<string> allowed = new HashSet<string>(StringComparer.Ordinal);
        IReadOnlyList<string> noNeedles = [];

        IReadOnlyList<string> ScanJson(string sample, IReadOnlyList<string>? needles = null) =>
            HardwareReplayAnonymizationRules.Scan("sample.json", sample, allowed, isDocumentation: false, needles ?? noNeedles);

        void MustReject(string gap, string sample, IReadOnlyList<string>? needles = null) =>
            Check.True(ScanJson(sample, needles).Count > 0, $"The anonymization scan missed ({gap}): {sample}");

        void MustAccept(string what, string sample)
        {
            var problems = ScanJson(sample);
            Check.True(problems.Count == 0, $"The anonymization scan must accept {what}: {string.Join("; ", problems)}");
        }

        // M2: shapes the first version of the scan did not know.
        MustReject("dashed GUID", "{\"id\":\"01234567-89ab-cdef-0123-456789abcdef\"}");
        MustReject("dashed GUID in braces, upper case", "{\"id\":\"{01234567-89AB-CDEF-0123-456789ABCDEF}\"}");
        MustReject("dashed GUID starting with f231", "{\"id\":\"f2310123-4567-89ab-cdef-0123456789ab\"}");
        MustAccept("a dashed form of a synthetic ID", "{\"id\":\"f231a001-0000-0000-0000-0000000000a1\"}");
        MustReject("e-mail address", "{\"contact\":\"someone@example.invalid\"}");
        MustReject("yyyyMMdd date", "{\"day\":\"20260615\"}");
        MustReject("yyyy/MM/dd date", "{\"day\":\"2026/06/15\"}");
        MustReject("yyyy.MM.dd date", "{\"day\":\"2026.06.15\"}");
        MustReject("EXIF-form date", "{\"taken\":\"2026:06:15 05:05:32\"}");
        MustReject("date in another year", "{\"day\":\"2025-01-15\"}");
        MustReject("yyyyMMdd date in another year", "{\"day\":\"20250115\"}");
        MustReject("Japanese-form date", "{\"day\":\"2026年6月15日\"}");
        MustAccept("slash, EXIF and ISO dates inside the shifted month", "{\"b\":\"2026/01/05\",\"c\":\"2026:01:05 05:05:32\",\"d\":\"2026-01-05T05:05:32Z\"}");
        MustReject("DESKTOP- host name", "{\"note\":\"DESKTOP-ABCDEFG\"}");
        MustReject("LAPTOP- host name", "{\"note\":\"LAPTOP-1234ABCD\"}");
        MustReject("WIN- host name", "{\"note\":\"WIN-ABCDEFGHIJK\"}");
        MustReject("host name under a host key", "{\"computerName\":\"anything\"}");
        MustReject("account under a user key", "{\"loginUserName\":\"anything\"}");
        MustReject("owner field", "{\"cameraOwner\":\"anything\"}");
        MustReject("body serial under a neutral key (letters and digits)", "{\"deviceKey\":\"AB12345\"}");
        MustReject("body serial under a neutral key (7 digits, quoted)", "{\"id\":\"1234567\"}");
        MustReject("body serial under a neutral key (7 digits, number)", "{\"id\":1234567}");
        MustAccept("the image size value and short counters", "{\"pixelDimensions\":\"7360x4912\",\"bytes\":703,\"n\":12345}");
        MustReject("10-digit epoch seconds", "{\"t\":1700000000}");
        MustReject("10-digit epoch seconds as text", "{\"t\":\"1700000000\"}");
        MustReject("16-digit epoch microseconds", "{\"t\":1700000000123456}");

        // "f231" is no longer enough on its own: the whole synthetic shape is required.
        MustReject("real-looking 32-digit value beginning f231", "{\"transactionId\":\"f231" + "0123456789abcdef0123456789ab\"}");
        MustReject("32-digit value beginning f231 with a free tail", "{\"transactionId\":\"f231a001000000000000123456789abc\"}");
        MustReject("real-looking 64-digit value beginning f231", "{\"sha256\":\"f231" + new string('7', 60) + "\"}");
        MustAccept(
            "the synthetic IDs the fixtures use",
            "{\"a\":\"" + HardwareReplayFixtures.SingleTransactionId + "\",\"b\":\"" + HardwareReplayFixtures.DualSucceededTransactionId +
            "\",\"c\":\"f231" + new string('0', 58) + "f1\"}");

        // Values read from the environment while the test runs (nothing is stored in the repo).
        var runtimeNeedles = HardwareReplayAnonymizationRules.RuntimeEnvironmentNeedles();
        void MustHitEnvironment(string what, string value)
        {
            var problems = ScanJson("{\"note\":\"seen on " + value + " today\"}", runtimeNeedles);
            Check.True(problems.Any(problem => problem.Contains("PC name or user profile", StringComparison.Ordinal)),
                $"The environment check missed {what}.");
        }
        var machine = Environment.MachineName;
        if (machine.Length >= 4) MustHitEnvironment("this PC's name", machine);
        var profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        Check.True(profile.Length >= 4, "The test needs a user profile path to check against.");
        MustHitEnvironment("this user's profile path", profile.Replace("\\", "\\\\", StringComparison.Ordinal));
        MustHitEnvironment("this user's profile path with forward slashes", profile.Replace('\\', '/'));
        if (machine.Length >= 4) MustHitEnvironment("this PC's name in another case", machine.ToUpperInvariant());
        MustReject("a supplied needle", "{\"note\":\"seen on fictional-host-77 today\"}", ["fictional-host-77"]);
        MustAccept("text without the supplied needle", "{\"note\":\"seen on another host today\"}");

        // M1: images. Start from a real dummy image of the fixtures and change one thing at a time.
        var good = HardwareReplayFixtures.ReadJpeg("single-cam-a");
        Check.True(HardwareReplayJpegContracts.Inspect("good", good).Count == 0, "The unchanged dummy image must pass the structure check.");

        void ImageMustFail(string gap, byte[] bytes) =>
            Check.True(HardwareReplayJpegContracts.Inspect(gap, bytes).Count > 0, $"The dummy-image check missed: {gap}");

        ImageMustFail("APP1 (EXIF)", InsertSegment(good, 0xE1, Encoding.ASCII.GetBytes("Exif\0\0")));
        ImageMustFail("APP2 (ICC or similar)", InsertSegment(good, 0xE2, Encoding.ASCII.GetBytes("ICC_PROFILE\0")));
        ImageMustFail("APP13 (IPTC or similar)", InsertSegment(good, 0xED, Encoding.ASCII.GetBytes("Photoshop 3.0\0")));
        ImageMustFail("progressive frame header", InsertSegment(good, 0xC2, new byte[15]));
        ImageMustFail("a second notice", InsertSegment(good, 0xFE, Encoding.ASCII.GetBytes(NoticeOf(good))));
        ImageMustFail("notice with extra text after it", ReplaceComment(good, NoticeOf(good) + " extra"));
        ImageMustFail("notice inside other text", ReplaceComment(good, "prefix " + NoticeOf(good)));
        ImageMustFail("a different COM text", ReplaceComment(good, "a comment"));
        ImageMustFail("a large table segment", InsertSegment(good, 0xDB, new byte[198]));
        ImageMustFail("33 bytes of image data", AppendImageData(good, 33));
        ImageMustFail("not a JPEG", Enumerable.Repeat((byte)'x', 300).ToArray());
        Check.True(HardwareReplayJpegContracts.Inspect(
                "limit", WithImageDataLength(good, HardwareReplayJpegContracts.MaxScanDataBytes)).Count == 0,
            "Exactly the allowed amount of image data must pass.");
        ImageMustFail("one byte over the limit", WithImageDataLength(good, HardwareReplayJpegContracts.MaxScanDataBytes + 1));

        // M1: the decoded bytes go through the text rules too. These pass the structure check
        // (few bytes of image data) and are caught only by scanning the decoded result.
        foreach (var (gap, planted) in new[]
                 {
                     ("a serial label in the image data", "Serial 1"),
                     ("an e-mail address in the image data", "a@b.example"),
                     ("a PC name in the image data", "DESKTOP-ABCDEFG"),
                     ("a 7-digit number in the image data", "1234567"),
                 })
        {
            var hidden = WithImageText(good, planted);
            Check.True(HardwareReplayJpegContracts.Inspect(gap, hidden).Count == 0,
                $"Sanity: the structure check alone should not see {gap}.");
            Check.True(HardwareReplayAnonymizationRules.Scan(
                    "images/single-cam-a.jpg.b64", Convert.ToBase64String(hidden), allowed, isDocumentation: false, noNeedles).Count > 0,
                $"The decoded image must be scanned: {gap}");
        }

        // M1: the .b64 route is closed. Wrong places and unreadable content fail instead of being skipped.
        var goodB64 = Convert.ToBase64String(good);
        Check.True(HardwareReplayAnonymizationRules.Scan(
                "images/single-cam-a.jpg.b64", goodB64, allowed, isDocumentation: false, noNeedles).Count == 0,
            "A valid dummy image in images/ must pass.");
        foreach (var misplaced in new[] { "dual-camera/hidden.b64", "images/x.png.b64", "images/sub/x.jpg.b64", "x.jpg.b64" })
        {
            Check.True(HardwareReplayAnonymizationRules.Scan(misplaced, goodB64, allowed, isDocumentation: false, noNeedles).Count > 0,
                $"A base64 file outside images/*.jpg.b64 must fail: {misplaced}");
        }
        foreach (var unreadable in new[] { "not base64 !!", "", "@@@@" })
        {
            Check.True(HardwareReplayAnonymizationRules.Scan(
                    "images/single-cam-a.jpg.b64", unreadable, allowed, isDocumentation: false, noNeedles).Count > 0,
                "A .b64 file that is not base64 must fail, not be skipped.");
        }
        Check.True(HardwareReplayAnonymizationRules.Scan(
                "images/other.jpg.b64", Convert.ToBase64String(Encoding.UTF8.GetBytes(new string('x', 300))),
                allowed, isDocumentation: false, noNeedles).Count > 0,
            "Base64 of something that is not a dummy JPEG must fail.");

        var folderRoot = HardwareReplayFixtures.NewTestRoot();
        try
        {
            System.IO.Directory.CreateDirectory(Path.Combine(folderRoot, "dual-camera"));
            File.WriteAllText(Path.Combine(folderRoot, "dual-camera", "hidden.b64"), goodB64);
            Check.True(HardwareReplayAnonymizationRules.CheckFileSet(folderRoot).Count > 0,
                "A .b64 file outside images/ must fail the file-set check.");
            File.Delete(Path.Combine(folderRoot, "dual-camera", "hidden.b64"));
            System.IO.Directory.CreateDirectory(Path.Combine(folderRoot, "images"));
            File.WriteAllText(Path.Combine(folderRoot, "images", "note.json"), "{}");
            Check.True(HardwareReplayAnonymizationRules.CheckFileSet(folderRoot).Count > 0,
                "Anything but *.jpg.b64 inside images/ must fail the file-set check.");
        }
        finally
        {
            HardwareReplayFixtures.DeleteTestRoot(folderRoot);
        }
    }

    // GitHub Issue #243: the gaps the second security audit found (the file names, images inside
    // JSON strings, digit runs next to letters, the date forms and the JPEG tables). Every input is
    // invented. Where a rule is narrowed, the synthetic shapes the fixtures use must still pass.
    private static void AnonymizationRulesCloseTheGapsOfTheSecondAudit()
    {
        IReadOnlySet<string> allowed = new HashSet<string>(StringComparer.Ordinal);
        IReadOnlyList<string> noNeedles = [];

        IReadOnlyList<string> ScanJson(string sample, IReadOnlyList<string>? needles = null) =>
            HardwareReplayAnonymizationRules.Scan("sample.json", sample, allowed, isDocumentation: false, needles ?? noNeedles);

        void MustReject(string gap, string sample, string? messagePart = null)
        {
            var problems = ScanJson(sample);
            Check.True(problems.Count > 0, $"The anonymization scan missed ({gap}): {sample}");
            if (messagePart is not null)
                Check.True(problems.Any(problem => problem.Contains(messagePart, StringComparison.Ordinal)),
                    $"The anonymization scan rejected ({gap}) for another reason than '{messagePart}': {string.Join("; ", problems)}");
        }

        void MustAccept(string what, string sample)
        {
            var problems = ScanJson(sample);
            Check.True(problems.Count == 0, $"The anonymization scan must accept {what}: {string.Join("; ", problems)}");
        }

        // M-2: the names of the files are published too.
        var folderRoot = HardwareReplayFixtures.NewTestRoot();
        try
        {
            foreach (var (gap, relative) in new[]
                     {
                         ("13-digit epoch in a file name", "single-camera/events-1700000000123.json"),
                         ("10-digit epoch in a directory name", "dual-camera/1700000000/events.json"),
                         ("hexadecimal run in a file name", "single-camera/0123456789abcdef0123.json"),
                         ("32-digit hexadecimal ID in a directory name", "0123456789abcdef0123456789abcdef/x.json"),
                         ("date in a file name", "single-camera/events-2025-06-15.json"),
                         ("compact date in a file name", "single-camera/events-20250615.json"),
                         ("7-digit number in a file name", "single-camera/cam1234567.json"),
                         ("e-mail address in a file name", "single-camera/someone@example.invalid.json"),
                     })
            {
                var path = Path.Combine(folderRoot, relative.Replace('/', Path.DirectorySeparatorChar));
                System.IO.Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(path, "{}");
                var problems = HardwareReplayAnonymizationRules.CheckFileSet(folderRoot, noNeedles);
                Check.True(problems.Any(problem => problem.Contains("(file name)", StringComparison.Ordinal)),
                    $"The file-set check missed ({gap}).");
                File.Delete(path);
                System.IO.Directory.Delete(Path.GetDirectoryName(path)!, recursive: false);
            }
            foreach (var relative in new[]
                     {
                         "single-camera/transaction.json",
                         "dual-camera/failed-cam-a/pair-journal.json",
                         "dual-camera/succeeded/diagnostics-cam-a.events.jsonl",
                         "images/single-cam-a.jpg.b64",
                         "single-camera/events-2026-01-05.json",
                         "f231a0010000000000000000000000a1/x.json",
                     })
            {
                Check.True(
                    HardwareReplayAnonymizationRules.ScanRelativePath(relative, noNeedles).Count == 0,
                    $"The file-name check must accept {relative}.");
            }
            var named = Path.Combine(folderRoot, "dual-camera");
            System.IO.Directory.CreateDirectory(named);
            File.WriteAllText(Path.Combine(named, "fictional-host-77.json"), "{}");
            Check.True(
                HardwareReplayAnonymizationRules.CheckFileSet(folderRoot, ["fictional-host-77"])
                    .Any(problem => problem.Contains("PC name or user profile", StringComparison.Ordinal)),
                "The PC name of this machine must be looked for in the file names too.");
        }
        finally
        {
            HardwareReplayFixtures.DeleteTestRoot(folderRoot);
        }

        // M-3: base64 inside JSON strings. Start from a dummy image of the fixtures.
        var good = HardwareReplayFixtures.ReadJpeg("single-cam-a");
        var goodB64 = Convert.ToBase64String(good);
        var notAnImage = Convert.ToBase64String(Encoding.UTF8.GetBytes(new string('x', 120)));
        var fakeJpeg = new byte[300];
        fakeJpeg[0] = 0xFF; fakeJpeg[1] = 0xD8; fakeJpeg[2] = 0xFF; fakeJpeg[3] = 0xE0;
        fakeJpeg[^2] = 0xFF; fakeJpeg[^1] = 0xD9;
        var fakeJpegB64 = Convert.ToBase64String(fakeJpeg);
        MustAccept("a dummy image under a *Base64 key", "{\"frameJpegBase64\":\"" + goodB64 + "\"}");
        MustAccept("a dummy image under a *Base64 key (other spelling)", "{\"frameBase64\":\"" + goodB64 + "\"}");
        MustAccept("an empty value under a *Base64 key", "{\"frameBase64\":\"\",\"other\":null}");
        MustAccept("a dummy image with JSON-escaped slashes", "{\"frameBase64\":\"" + goodB64.Replace("/", "\\/", StringComparison.Ordinal) + "\"}");
        MustAccept("a dummy image under a neutral key", "{\"frame\":\"" + goodB64 + "\"}");
        MustReject("a JPEG-looking image under a *Base64 key", "{\"frameJpegBase64\":\"" + fakeJpegB64 + "\"}", "embedded base64");
        MustReject("a JPEG-looking image under a neutral key", "{\"preview\":\"" + fakeJpegB64 + "\"}", "embedded base64");
        MustReject("a short value under a *Base64 key", "{\"thumbBase64\":\"AAAA\"}");
        MustReject("text that is not an image under a *Base64 key", "{\"frameBase64\":\"" + notAnImage + "\"}");
        MustReject("text that is not an image in a long neutral value", "{\"note\":\"" + notAnImage + "\"}");
        MustReject("a number under a *Base64 key", "{\"frameBase64\":12345}", "must be a string");
        MustReject("a URL-safe base64 image", "{\"x\":\"" + fakeJpegB64.Replace('+', '-').Replace('/', '_') + "\"}");
        MustReject("a base64 image wrapped at 76 characters",
            "{\"x\":\"" + string.Join("\\n", Enumerable.Range(0, (fakeJpegB64.Length + 75) / 76)
                .Select(i => fakeJpegB64.Substring(i * 76, Math.Min(76, fakeJpegB64.Length - i * 76)))) + "\"}");
        MustReject("a base64 string that cannot be decoded", "{\"x\":\"" + new string('A', 65) + "\"}");
        MustReject("a dummy image with one more byte of text in the image data",
            "{\"frameBase64\":\"" + Convert.ToBase64String(WithImageText(good, "Serial 1")) + "\"}");
        MustAccept("a long prose value with spaces", "{\"detail\":\"" + string.Join(' ', Enumerable.Repeat("camera", 14)) + "\"}");
        MustAccept("a 64-digit synthetic hexadecimal value", "{\"sha256\":\"f231" + new string('0', 58) + "f1\"}");

        // M-4: a digit run next to a letter or a hyphen, and the synthetic IDs as whole shapes.
        MustReject("body serial with a prefix", "{\"id\":\"CAM-A-1234567\"}", "7-digit number");
        MustReject("body serial glued to letters before", "{\"id\":\"SN1234567\"}", "7-digit number");
        MustReject("body serial glued to letters after", "{\"id\":\"1234567ABC\"}", "7-digit number");
        MustReject("digit run behind a hyphen", "{\"id\":\"cam-1234567-x\"}", "7-digit number");
        MustReject("a run ID with a prefix", "{\"id\":\"x-run-231001-1\"}", "6-digit number");
        MustReject("a run ID with a letter behind", "{\"id\":\"run-231001-1x\"}", "6-digit number");
        MustReject("a run ID with a longer number", "{\"id\":\"run-2310011-1\"}", "7-digit number");
        MustReject("a run ID with a longer counter", "{\"id\":\"run-231001-12\"}", "6-digit number");
        MustReject("a run ID with another number behind", "{\"id\":\"hybrid-tx-231002-3-1234567\"}", "6-digit number");
        MustReject("a run ID outside the 231 range", "{\"id\":\"run-241001-1\"}", "6-digit number");
        MustReject("an sdkVersion-like value under another key", "{\"note\":\"0x3010000+windows-wpd-1\"}", "7-digit number");
        MustReject("an sdkVersion that is not one of the known values", "{\"sdkVersion\":\"0x3010000+other\"}", "7-digit number");
        MustAccept(
            "the synthetic run IDs",
            "{\"runId\":\"run-231001-1\",\"tx\":\"hybrid-tx-231002-3\",\"a\":\"dual-leg-CAM-A-run-231002-2\"," +
            "\"b\":\"dual-leg-CAM-B-run-231003-4\",\"p\":\"C:\\\\a0-replay-fixture\\\\run-231001-1\\\\hybrid-tx-231002-5\\\\CAM-A\"}");
        MustAccept(
            "the sdkVersion values the Agent wrote",
            "{\"sdkVersion\": \"0x3010000+windows-wpd-1;access=read-write;qos=impersonation;command-target=functional\"," +
            "\"sdkVersion\":\"Nikon-D810-licensed-dual-session\"}");
        MustAccept("a decimal fraction and a fractional-second time", "{\"ratio\":1234567.5,\"t\":\"2026-01-05T05:05:32.3127834Z\"}");

        // M-5 is the local script's; L-2: the shapes the first versions did not read.
        MustReject("date as M/d/yyyy", "{\"day\":\"6/15/2026\"}", "date at offset");
        MustReject("date as d.M.yyyy", "{\"day\":\"15.06.2026\"}", "date at offset");
        MustReject("date as d/M/yyyy in another year", "{\"day\":\"15/1/2025\"}", "date at offset");
        MustReject("date as RFC 1123", "{\"day\":\"Mon, 15 Jun 2026 05:05:32 GMT\"}", "date at offset");
        MustReject("date with a month name first", "{\"day\":\"June 15, 2026\"}", "date at offset");
        MustReject("date with one-digit parts", "{\"day\":\"2026-6-15\"}", "date at offset");
        MustReject("date with one-digit parts and slashes", "{\"day\":\"2026/6/5\"}", "date at offset");
        MustReject("a camera file name with a date and time", "{\"file\":\"DSC_20260615050532\"}", "date and time at offset");
        MustAccept("one-digit and RFC 1123 dates inside the shifted month",
            "{\"a\":\"1/5/2026\",\"b\":\"5.1.2026\",\"c\":\"2026-1-5\",\"d\":\"Thu, 15 Jan 2026 05:05:32 GMT\",\"e\":\"Jan 15, 2026\"}");
        MustReject("Windows SID", "{\"u\":\"S-1-5-21-12-34-56\"}", "Windows SID");
        MustReject("Windows SID with a relative ID", "{\"u\":\"S-1-5-21-12-34-56-1001\"}", "Windows SID");
        MustReject("UNC server name with doubled backslashes", "{\"p\":\"\\\\\\\\fileserver\\\\share\\\\x\"}", "UNC path");
        MustReject("UNC server name", "{\"p\":\"\\\\fileserver\\share\"}", "UNC path");
        MustReject("UNC server name with slashes", "{\"p\":\"//fileserver/share/x\"}", "UNC path");
        MustAccept("a double slash inside a value and the synthetic path", "{\"u\":\"key:value//x/y\",\"p\":\"C:\\\\a0-replay-fixture\\\\x\"}");
        MustReject("lower-case DESKTOP- host name", "{\"n\":\"desktop-abcdefg\"}", "host name");
        MustReject("mixed-case DESKTOP- host name", "{\"n\":\"Desktop-AbCdEfG\"}", "host name");
        MustReject("lower-case WIN- host name", "{\"n\":\"win-abcdefghijk\"}", "host name");
        MustAccept("runtime identifiers", "{\"a\":\"win-x64\",\"b\":\"win-arm64\"}");
        MustReject("an artist key", "{\"artist\":\"anything\"}", "artist, author, host or account field");
        MustReject("a host key", "{\"host\":\"anything\"}", "artist, author, host or account field");
        MustReject("an author key in capitals", "{\"AUTHOR\":\"anything\"}", "artist, author, host or account field");
        MustReject("a copyright key", "{\"imageCopyright\":\"anything\"}", "artist, author, host or account field");
        MustAccept("a key that only contains 'host'", "{\"ghost\":\"anything\"}");

        // L-2: the tables and headers of the dummy image are compared byte for byte.
        void ImageMustFail(string gap, byte[] bytes) =>
            Check.True(HardwareReplayJpegContracts.Inspect(gap, bytes).Count > 0, $"The dummy-image check missed: {gap}");

        byte[] WithByteChanged(byte marker, int offsetInPayload)
        {
            var copy = (byte[])good.Clone();
            copy[SegmentStart(copy, marker) + 4 + offsetInPayload] ^= 0x01;
            return copy;
        }

        Check.True(HardwareReplayJpegContracts.Inspect("good", good).Count == 0, "The unchanged dummy image must pass the structure check.");
        ImageMustFail("one byte changed in a quantization table", WithByteChanged(0xDB, 10));
        ImageMustFail("one byte changed in the last byte of a quantization table", WithByteChanged(0xDB, 64));
        ImageMustFail("one byte changed in a Huffman table", WithByteChanged(0xC4, 5));
        ImageMustFail("one byte changed in a Huffman value list", WithByteChanged(0xC4, 20));
        ImageMustFail("one byte changed in the JFIF header", WithByteChanged(0xE0, 9));
        ImageMustFail("one byte changed in the frame header", WithByteChanged(0xC0, 9));
        ImageMustFail("one byte changed in the scan header", WithByteChanged(0xDA, 3));
        var firstTable = good[(SegmentStart(good, 0xDB) + 4)..(SegmentStart(good, 0xDB) + 4 + 65)];
        ImageMustFail("a repeated quantization table", InsertSegment(good, 0xDB, firstTable));
    }

    private static string NoticeOf(byte[] jpeg)
    {
        var start = SegmentStart(jpeg, 0xFE);
        var length = (jpeg[start + 2] << 8) | jpeg[start + 3];
        return Encoding.ASCII.GetString(jpeg, start + 4, length - 2);
    }

    private static int SegmentStart(byte[] jpeg, byte marker)
    {
        for (var index = 2; index + 4 < jpeg.Length;)
        {
            if (jpeg[index + 1] == marker) return index;
            index += 2 + ((jpeg[index + 2] << 8) | jpeg[index + 3]);
        }
        throw new InvalidDataException($"The image has no 0x{marker:X2} segment.");
    }

    private static byte[] Segment(byte marker, byte[] payload)
    {
        var length = payload.Length + 2;
        return [0xFF, marker, (byte)(length >> 8), (byte)(length & 0xFF), .. payload];
    }

    // Inserts a segment right after SOI.
    private static byte[] InsertSegment(byte[] jpeg, byte marker, byte[] payload) =>
        [.. jpeg[..2], .. Segment(marker, payload), .. jpeg[2..]];

    private static byte[] ReplaceComment(byte[] jpeg, string comment)
    {
        var start = SegmentStart(jpeg, 0xFE);
        var length = (jpeg[start + 2] << 8) | jpeg[start + 3];
        return [.. jpeg[..start], .. Segment(0xFE, Encoding.ASCII.GetBytes(comment)), .. jpeg[(start + 2 + length)..]];
    }

    private static int ImageDataLength(byte[] jpeg)
    {
        var start = SegmentStart(jpeg, 0xDA);
        var length = (jpeg[start + 2] << 8) | jpeg[start + 3];
        return jpeg.Length - 2 - (start + 2 + length);
    }

    private static byte[] AppendImageData(byte[] jpeg, int extra) =>
        [.. jpeg[..^2], .. new byte[extra], 0xFF, 0xD9];

    private static byte[] WithImageDataLength(byte[] jpeg, int total)
    {
        var start = SegmentStart(jpeg, 0xDA);
        var length = (jpeg[start + 2] << 8) | jpeg[start + 3];
        return [.. jpeg[..(start + 2 + length)], .. new byte[total], 0xFF, 0xD9];
    }

    private static byte[] WithImageText(byte[] jpeg, string text)
    {
        var start = SegmentStart(jpeg, 0xDA);
        var length = (jpeg[start + 2] << 8) | jpeg[start + 3];
        return [.. jpeg[..(start + 2 + length)], .. Encoding.ASCII.GetBytes(text), 0xFF, 0xD9];
    }

    private static void OperatorInputsAndDurableStateLoadThroughProductionReaders()
    {
        var root = HardwareReplayFixtures.NewTestRoot();
        try
        {
            // The operator-approved profile file the real run used goes through the strict loader.
            var profile = FixtureProfile(root);
            Check.Equal("Nikon D810", profile.CameraModel);
            Check.Equal("7360x4912", profile.PixelDimensions);

            // The app's own durable snapshot after a finished transaction: no pending request, the
            // binding invalidation reason written as the name "None" (the Agent writes "").
            var stateDirectory = Path.Combine(root, "recovery-state");
            System.IO.Directory.CreateDirectory(stateDirectory);
            File.Copy(
                Path.Combine(HardwareReplayFixtures.Directory, "dual-camera", "recovery-state", "pending-capture-recovery-only.json"),
                Path.Combine(stateDirectory, "pending-capture-recovery-only.json"));
            var snapshot = new CaptureRecoveryOnlyTransactionSnapshotStore(root).Load();
            Check.True(snapshot is not null, "The real durable snapshot must load.");
            Check.True(snapshot!.PendingRequest is null, "A finished transaction's snapshot has no pending request.");
            Check.Equal(DualBindingInvalidationReason.None, snapshot.BindingInvalidationReason);
        }
        finally
        {
            HardwareReplayFixtures.DeleteTestRoot(root);
        }
    }

    private static async Task<(HardwareSingleCameraViewModel ViewModel, ReplaySingleCameraOperations Operations)>
        PrepareSingleAsync(string root, ReplaySingleLanding landing)
    {
        var artifacts = Path.Combine(root, "phase0", "camera-agent", "artifacts");
        var operations = new ReplaySingleCameraOperations(artifacts, landing);
        var viewModel = new HardwareSingleCameraViewModel(
            operations,
            new HardwareSingleAppStateStore(Path.Combine(root, "state")),
            new HardwareOriginalExporter(Path.Combine(root, "export")),
            new MutableTimeProvider(DateTimeOffset.Parse("2026-01-01T05:05:44Z")));
        await viewModel.InitializeAsync();
        viewModel.ExclusiveCameraControlConfirmed = true;
        await viewModel.CheckReadinessAsync();
        viewModel.DedicatedSpoolScopeConfirmed = true;
        viewModel.ExactObjectDeleteConfirmed = true;
        Check.True(viewModel.CanCapture,
            "The real D810 readiness (fileType not advertised) must let the single-camera flow capture. " +
            $"blocker: {viewModel.BlockerText}");
        return (viewModel, operations);
    }

    // The real D810 observation (fileType not advertised, focusMode only the opaque value 1) goes
    // through the app's own profile approval and must produce the settings of the profile the real
    // run stored. This is the fake-versus-real check for those two properties: a body that reports
    // them differently would not be approvable.
    private static async Task SingleCameraProfileApprovalReproducesTheRealApprovedProfileAsync()
    {
        var root = HardwareReplayFixtures.NewTestRoot();
        try
        {
            var operations = new ReplaySingleCameraOperations(Path.Combine(root, "artifacts"), ReplaySingleLanding.AppTransactionFolder);
            var readiness = await operations.GetReadinessAsync("CAM-A");
            var approvedAt = DateTimeOffset.Parse("2026-01-01T05:04:50Z");
            var path = Path.Combine(root, "approved-single-capture-profile.json");
            await new HardwareSingleCaptureProfileStore(path, new MutableTimeProvider(approvedAt))
                .ApproveCamAAsync(readiness.Payload.ObservedSettings);

            using var produced = JsonDocument.Parse(File.ReadAllText(path));
            using var real = JsonDocument.Parse(HardwareReplayFixtures.ReadText("single-camera/approved-capture-profile.json"));
            Check.True(
                System.Text.Json.Nodes.JsonNode.DeepEquals(
                    System.Text.Json.Nodes.JsonNode.Parse(produced.RootElement.GetProperty("expectedSettings").GetRawText()),
                    System.Text.Json.Nodes.JsonNode.Parse(real.RootElement.GetProperty("expectedSettings").GetRawText())),
                "The app must turn the real D810 observation into exactly the expected settings of the real approved profile.");
            foreach (var name in new[] { "schemaVersion", "profileVersion", "selectedAlias", "cameraMode", "approved", "approvedBy", "approvedAtUtc", "expiresAtUtc" })
            {
                Check.Equal(real.RootElement.GetProperty(name).GetRawText(), produced.RootElement.GetProperty(name).GetRawText());
            }
        }
        finally
        {
            HardwareReplayFixtures.DeleteTestRoot(root);
        }
    }

    private static async Task SingleCameraReplaySavesThroughViewModelAsync()
    {
        var root = HardwareReplayFixtures.NewTestRoot();
        try
        {
            var (viewModel, operations) = await PrepareSingleAsync(root, ReplaySingleLanding.AppTransactionFolder);
            using var _ = viewModel;
            await viewModel.CaptureAsync();
            Check.Equal(1, operations.CaptureCalls);
            Check.True(viewModel.CanExport,
                $"The replayed real capture must be exportable. summary: {viewModel.RetainedOriginalSummary} / {viewModel.TechnicalDetail}");
            Check.True(viewModel.RetainedOriginalSummary.Contains("SHA-256確認済み", StringComparison.Ordinal),
                $"The app must re-verify the replayed original. Actual: {viewModel.RetainedOriginalSummary}");
            Check.False(viewModel.TechnicalDetail.Contains("original_reread_failed", StringComparison.Ordinal),
                "The app's re-read of the replayed original must not fail (#216).");

            await viewModel.ExportAsync();
            Check.True(viewModel.LastExportPath.Length > 0 && File.Exists(viewModel.LastExportPath),
                $"The export must exist. summary: {viewModel.ExportSummary}");
            var dummy = HardwareReplayFixtures.ReadJpeg("single-cam-a");
            Check.True(File.ReadAllBytes(viewModel.LastExportPath).SequenceEqual(dummy),
                "The exported file must be a byte-identical copy of the replayed original.");
            Check.True(Path.GetFileName(viewModel.LastExportPath).StartsWith("A0-single-20260101-050544000-CAM-A-", StringComparison.Ordinal),
                $"Unexpected export name: {Path.GetFileName(viewModel.LastExportPath)}");
            Check.Equal(1, System.IO.Directory.GetFiles(Path.Combine(root, "export")).Length);
        }
        finally
        {
            HardwareReplayFixtures.DeleteTestRoot(root);
        }
    }

    // The field run behind #216: the Agent filed the original under its own hybrid-tx-... folder, so
    // the app's provenance check (exact <run>/<app transaction>/<alias>/original.jpg) refused it
    // and showed original_reread_failed. The refusal itself is correct and must stay; what changed
    // in #216 is that the real Agent no longer lands there (covered by the C++ replay).
    private static async Task SingleCameraLegacyLandingIsRejectedByViewModelAsync()
    {
        var root = HardwareReplayFixtures.NewTestRoot();
        try
        {
            var (viewModel, operations) = await PrepareSingleAsync(root, ReplaySingleLanding.LegacyHybridFolder);
            using var _ = viewModel;
            await viewModel.CaptureAsync();
            Check.True(operations.LastOriginalPath!.Contains("hybrid-tx-", StringComparison.Ordinal),
                "The legacy landing must use the real hybrid-tx folder naming.");
            Check.False(viewModel.CanExport, "An original filed outside the app's transaction folder must not be exportable.");
            Check.True(viewModel.TechnicalDetail.Contains("original_reread_failed", StringComparison.Ordinal),
                $"The refusal must be reported as original_reread_failed. Actual: {viewModel.TechnicalDetail}");
            Check.False(System.IO.Directory.Exists(Path.Combine(root, "export")) &&
                        System.IO.Directory.GetFiles(Path.Combine(root, "export")).Length > 0,
                "Nothing may be written to the export folder.");
        }
        finally
        {
            HardwareReplayFixtures.DeleteTestRoot(root);
        }
    }

    internal static (DualCameraIdentitySnapshot Identity, DateTimeOffset Started) FixtureIdentity(ReplayDualScenario scenario)
    {
        using var document = JsonDocument.Parse(HardwareReplayFixtures.DualTerminalResult(scenario));
        var evidence = document.RootElement.GetProperty("evidence");
        var snapshot = evidence.GetProperty("identitySnapshot");
        return (
            new DualCameraIdentitySnapshot(
                DualCameraIdentityStatus.Ready,
                snapshot.GetProperty("reasonCode").GetString()!,
                DateTimeOffset.Parse(snapshot.GetProperty("observedAtUtc").GetString()!),
                DateTimeOffset.Parse(snapshot.GetProperty("expiresAtUtc").GetString()!)),
            DateTimeOffset.Parse(evidence.GetProperty("watchdogStartedAtUtc").GetString()!));
    }

    internal static HardwareDualCaptureRecoveryOnlyProfile FixtureProfile(string root)
    {
        var profilePath = Path.Combine(root, "approved-capture-recovery-only-profile.json");
        File.Copy(
            Path.Combine(HardwareReplayFixtures.Directory, "dual-camera", "operator-inputs", "approved-capture-recovery-only-profile.json"),
            profilePath,
            overwrite: true);
        return HardwareDualCaptureRecoveryOnlyProfileFile.Load(profilePath);
    }

    private static async Task DualCameraSucceededReplaySavesThroughWorkflowAndExporterAsync()
    {
        var root = HardwareReplayFixtures.NewTestRoot();
        try
        {
            var (identity, started) = FixtureIdentity(ReplayDualScenario.Succeeded);
            var clock = new MutableTimeProvider(started);
            var transport = new ReplayDualHardwareTransport(ReplayDualScenario.Succeeded);
            var operations = new DualHardwareCameraAgentOperations(transport);
            var workflow = new HardwareDualCaptureRecoveryOnlyWorkflow(
                Path.Combine(root, "products"), operations, operations, FixtureProfile(root), clock);

            var outcome = await workflow.CaptureAsync(identity);
            Check.True(outcome.Succeeded,
                $"The replayed real Succeeded terminal must be accepted. terminal={outcome.TerminalState} failure={outcome.FailureCode} detail={outcome.FailureReason}");
            Check.Equal(DualBindingInvalidationReason.None, outcome.BindingInvalidationReason);
            Check.Equal(0, outcome.AutomaticRetryCount);
            Check.False(outcome.RecoveryPending, "A validated terminal result must not stay pending.");
            Check.False(workflow.HasPendingRecovery, "The durable snapshot must be cleared.");
            Check.True(outcome.Originals.Select(item => item.Alias).SequenceEqual(["CAM-A", "CAM-B"]),
                "Both originals must come back in CAM-A, CAM-B order.");
            Check.True(outcome.Originals[0].Sha256 == HardwareReplayFixtures.Sha256(HardwareReplayFixtures.ReadJpeg("dual-cam-a")) &&
                       outcome.Originals[1].Sha256 == HardwareReplayFixtures.Sha256(HardwareReplayFixtures.ReadJpeg("dual-cam-b")),
                "The app must re-verify exactly the originals the replayed Agent wrote.");
            Check.True(transport.Operations.SequenceEqual([
                    DualHardwareCameraAgentProtocol.Operations.GetCapabilities,
                    DualHardwareCameraAgentProtocol.Operations.ReservePairTransaction,
                    DualHardwareCameraAgentProtocol.Operations.StartReservedCaptureRecoveryOnly,
                ]),
                "One capture must be exactly: capabilities, reservation, one start. Actual: " + string.Join(",", transport.Operations));

            var exportDirectory = Path.Combine(root, "export");
            var exporter = new HardwareOriginalExporter(exportDirectory);
            var exported = await exporter.ExportDualOriginalsAsync(
                outcome.Originals,
                outcome.TransactionId.ToString("N"),
                outcome.TransactionDirectory,
                DateTimeOffset.Parse("2026-01-01T08:48:00Z"));
            Check.Equal(2, exported.Count);
            for (var index = 0; index < 2; index++)
            {
                var name = Path.GetFileName(exported[index]);
                Check.True(name.StartsWith("A0-dual-20260101-084800000-" + outcome.Originals[index].Alias + "-", StringComparison.Ordinal),
                    $"Unexpected dual export name: {name}");
                Check.True(File.ReadAllBytes(exported[index]).SequenceEqual(File.ReadAllBytes(outcome.Originals[index].Path)),
                    "The exported file must be a byte-identical copy of the verified original.");
            }

            // CAM-A only (the shape a FailedPartial CAM-A-only export takes) uses the same path.
            var onlyA = await exporter.ExportDualOriginalsAsync(
                [outcome.Originals[0]],
                outcome.TransactionId.ToString("N"),
                outcome.TransactionDirectory,
                DateTimeOffset.Parse("2026-01-01T08:49:00Z"));
            Check.Equal(1, onlyA.Count);
            Check.Equal(3, System.IO.Directory.GetFiles(exportDirectory).Length);
        }
        finally
        {
            HardwareReplayFixtures.DeleteTestRoot(root);
        }
    }

    // #224 and the second attempt of the real session: the first attempt failed at CAM-A before
    // any shutter (Failed/CaptureCameraA, zero originals, bindingInvalidationReason ""), and the
    // app could not read that terminal result, so the same-ID pending never cleared.
    private static async Task DualCameraFailedReplayClearsPendingAsync()
    {
        var root = HardwareReplayFixtures.NewTestRoot();
        try
        {
            var (identity, started) = FixtureIdentity(ReplayDualScenario.FailedCameraA);
            var profile = FixtureProfile(root);

            // (a) the start response arrives
            var directTransport = new ReplayDualHardwareTransport(ReplayDualScenario.FailedCameraA);
            var directOperations = new DualHardwareCameraAgentOperations(directTransport);
            var directWorkflow = new HardwareDualCaptureRecoveryOnlyWorkflow(
                Path.Combine(root, "direct"), directOperations, directOperations, profile, new MutableTimeProvider(started));
            var direct = await directWorkflow.CaptureAsync(identity);
            AssertFailedAtCameraA(direct, directWorkflow);

            // (b) the response is lost; the app asks the Agent again about the same transaction
            var lostTransport = new ReplayDualHardwareTransport(
                ReplayDualScenario.FailedCameraA, startResponseUnknown: true, firstQueryFails: true);
            var lostOperations = new DualHardwareCameraAgentOperations(lostTransport);
            var lostRoot = Path.Combine(root, "lost");
            var lostWorkflow = new HardwareDualCaptureRecoveryOnlyWorkflow(
                lostRoot, lostOperations, lostOperations, profile, new MutableTimeProvider(started));
            var pending = await lostWorkflow.CaptureAsync(identity);
            Check.True(pending.RecoveryPending && lostWorkflow.HasPendingRecovery,
                "A lost start response must leave the same-ID recovery pending.");
            var restarted = new HardwareDualCaptureRecoveryOnlyWorkflow(
                lostRoot, lostOperations, lostOperations, profile, new MutableTimeProvider(started));
            Check.True(restarted.HasPendingRecovery, "The pending transaction must survive an app restart.");
            var recovered = await restarted.RecoverAsync();
            AssertFailedAtCameraA(recovered, restarted);
            Check.Equal(1, lostTransport.Operations.Count(op => op == DualHardwareCameraAgentProtocol.Operations.StartReservedCaptureRecoveryOnly));

            // A terminal result without originals has nothing to save: refused before any write.
            var exportDirectory = Path.Combine(root, "export-none");
            await Check.ThrowsAsync<ExportSourceUnavailableException>(() =>
                new HardwareOriginalExporter(exportDirectory).ExportDualOriginalsAsync(
                    recovered.Originals,
                    recovered.TransactionId.ToString("N"),
                    recovered.TransactionDirectory,
                    DateTimeOffset.Parse("2026-01-01T05:44:00Z")));
            Check.False(System.IO.Directory.Exists(exportDirectory) &&
                        System.IO.Directory.GetFiles(exportDirectory).Length > 0,
                "Nothing may be written when there is no original.");
        }
        finally
        {
            HardwareReplayFixtures.DeleteTestRoot(root);
        }
    }

    private static void AssertFailedAtCameraA(
        HardwareDualCaptureRecoveryOnlyExecution outcome,
        HardwareDualCaptureRecoveryOnlyWorkflow workflow)
    {
        Check.Equal(DualHardwareCaptureTerminalState.Failed, outcome.TerminalState);
        Check.Equal(DualCameraFailureCode.CaptureCameraA, outcome.FailureCode);
        Check.Equal(0, outcome.Originals.Count);
        Check.Equal(DualBindingInvalidationReason.None, outcome.BindingInvalidationReason);
        Check.False(outcome.RecoveryPending, "The real Failed/CaptureCameraA terminal must release the same-ID gate (#224).");
        Check.False(workflow.HasPendingRecovery, "The durable pending snapshot must be cleared.");
    }

    // The real binding the Agent echoed back was valid for exactly five minutes (#221, second
    // attempt: it expired while the operator waited). The app enforces that window.
    private static async Task DualCameraIdentityExpiresAfterFiveMinutesAsync()
    {
        var root = HardwareReplayFixtures.NewTestRoot();
        try
        {
            var (identity, _) = FixtureIdentity(ReplayDualScenario.Succeeded);
            Check.Equal(TimeSpan.FromMinutes(5), identity.ExpiresAtUtc - identity.ObservedAtUtc);

            var transport = new ReplayDualHardwareTransport(ReplayDualScenario.Succeeded);
            var operations = new DualHardwareCameraAgentOperations(transport);
            var expired = new HardwareDualCaptureRecoveryOnlyWorkflow(
                Path.Combine(root, "products"), operations, operations, FixtureProfile(root),
                new MutableTimeProvider(identity.ExpiresAtUtc));
            var refused = await expired.CaptureAsync(identity);
            Check.Equal(DualCameraFailureCode.IdentityNotReady, refused.FailureCode);
            Check.Equal(0, transport.Operations.Count);

            var stillValid = new HardwareDualCaptureRecoveryOnlyWorkflow(
                Path.Combine(root, "products-inside"), operations, operations, FixtureProfile(root),
                new MutableTimeProvider(identity.ExpiresAtUtc - TimeSpan.FromSeconds(1)));
            var inside = await stillValid.CaptureAsync(identity);
            Check.True(inside.FailureCode != DualCameraFailureCode.IdentityNotReady,
                "One second before expiry the binding must still be accepted.");
        }
        finally
        {
            HardwareReplayFixtures.DeleteTestRoot(root);
        }
    }
}
