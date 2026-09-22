using System.Globalization;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation;
using A0CameraStitcher.M3.Foundation.DualCamera;

namespace A0CameraStitcher.M3.ReviewCli;

internal static class Program
{
    private const string AppId = "a0-camera-stitcher-review-cli";
    private const int ContractVersion = 2;
    private static readonly string Build = typeof(Program).Assembly
        .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "unavailable";
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);

    private sealed record Envelope(
        string RequestId,
        int Version,
        string Build,
        string Status,
        DateTimeOffset ObservedAtUtc,
        string Operation,
        object? Data,
        string? ErrorCode)
    {
        public string AppId => Program.AppId;
        public string Environment => "Windows-local standalone read-only";
    }

    private sealed record ReviewsArguments(string Root, int Offset, int Limit);
    private sealed record VerifyReviewArguments(string ProductRoot, string ResultId, string ExpectedKind);
    private sealed class ArtifactVerificationException : Exception { }

    public static async Task<int> Main(string[] args)
    {
        var requestId = Guid.NewGuid().ToString("N");
        var operation = args.FirstOrDefault() is "describe" or "reviews" or "verify-review" ? args[0] : "unknown";
        try
        {
            object data = operation switch
            {
                "describe" => Describe(args),
                "reviews" => await ReviewsAsync(ParseReviews(args), CancellationToken.None),
                "verify-review" => await VerifyReviewAsync(ParseVerifyReview(args), CancellationToken.None),
                _ => throw new ArgumentException("Unknown operation."),
            };
            Write(new Envelope(requestId, ContractVersion, Build, "ok", DateTimeOffset.UtcNow,
                operation, data, null));
            return 0;
        }
        catch (ArgumentException)
        {
            return Fail(requestId, operation, "invalid_input");
        }
        catch (DirectoryNotFoundException)
        {
            return Fail(requestId, operation, "observation_unavailable");
        }
        catch (FileNotFoundException)
        {
            return Fail(requestId, operation, "observation_unavailable");
        }
        catch (InvalidDataException)
        {
            return Fail(requestId, operation, "invalid_metadata");
        }
        catch (JsonException)
        {
            return Fail(requestId, operation, "invalid_metadata");
        }
        catch (ArtifactVerificationException)
        {
            return Fail(requestId, operation, "invalid_artifacts");
        }
        catch (UnauthorizedAccessException)
        {
            return Fail(requestId, operation, "access_denied");
        }
        catch (IOException)
        {
            return Fail(requestId, operation, "observation_unavailable");
        }
        catch (OperationCanceledException)
        {
            return Fail(requestId, operation, "observation_timeout");
        }
        catch
        {
            return Fail(requestId, operation, "observation_unavailable");
        }
    }

    private static object Describe(string[] args)
    {
        if (args.Length != 1) throw new ArgumentException("describe accepts no options.");
        return new
        {
            appId = AppId,
            contractVersion = ContractVersion,
            environment = "Windows-local standalone read-only",
            operations = new[] { "describe", "reviews", "verify-review" },
            inputs = new
            {
                reviews = "--root <existing operator-review absolute fixed-local path> [--offset 0..1000] [--limit 1..25]",
                verifyReview = "--product-root <absolute fixed-local path> --result-id <nonempty lowercase GUID N> --expected-kind <Product|Simulated>",
            },
            verifyOutput = "result and transaction IDs, declared review kind, manifest/output/original hashes and relative paths, dimensions, adapter hash; no image bytes",
            adapter = "fixed sibling A0CameraStitcher.M2Adapter.exe; no executable argument or environment override",
            compatibility = "version 2 adds read-only verification; clients must check the contract version and operation list",
            mutation = "not supported",
            guiInstance = "not used",
            artifactVerification = "verify-review only; not hardware or quality acceptance",
            limits = new { maxScanned = 1000, offset = "0..1000", limit = "1..25", defaultOffset = 0, defaultLimit = 25, verifyTimeoutSeconds = 40 },
            unavailableOperations = new[] { "accept", "capture" },
            authorization = "current Windows account filesystem read permissions; no elevation or GUI control",
            pagination = "offset is within each locked snapshot; concurrent changes can move later pages",
        };
    }

    private static async Task<object> ReviewsAsync(ReviewsArguments args, CancellationToken cancellationToken)
    {
        if (!OperatingSystem.IsWindows()) throw new ArgumentException("reviews requires Windows.");
        if (!Directory.Exists(args.Root)) throw new DirectoryNotFoundException();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(10));
        var store = new FileOperatorReviewStore(args.Root);
        var page = await store.QueryReadOnlyAsync(args.Offset, args.Limit, timeout.Token).ConfigureAwait(false);
        return new
        {
            targetFingerprint = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(args.Root.ToUpperInvariant()))),
            artifactVerification = "not performed",
            records = page.Records,
            nextOffset = page.NextOffset,
            total = page.Total,
        };
    }

    private static async Task<object> VerifyReviewAsync(VerifyReviewArguments args, CancellationToken cancellationToken)
    {
        if (!OperatingSystem.IsWindows()) throw new ArgumentException("verify-review requires Windows.");
        ValidateFixedLocalPath(args.ProductRoot, directory: true);
        var adapterPath = Path.Combine(AppContext.BaseDirectory, "A0CameraStitcher.M2Adapter.exe");
        ValidateFixedLocalPath(adapterPath, directory: false);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(40));
        var token = timeout.Token;
        using var adapterImage = new FileStream(adapterPath, FileMode.Open, FileAccess.Read, FileShare.Read,
            81920, FileOptions.Asynchronous | FileOptions.SequentialScan);
        var adapterHash = Convert.ToHexString(await SHA256.HashDataAsync(adapterImage, token).ConfigureAwait(false)).ToLowerInvariant();
        var store = new FileOperatorReviewStore(Path.Combine(args.ProductRoot, "operator-review"));
        var initial = await store.ReadOneReadOnlyAsync(args.ResultId, token).ConfigureAwait(false);
        if (initial.State != "Pending" || initial.ReviewKind != args.ExpectedKind)
            throw new InvalidDataException("Review is not an expected pending record.");
        HistoricalReviewArtifacts artifacts;
        try
        {
            var adapter = new M2OfflineStitcherProcessAdapter(adapterPath);
            artifacts = await adapter.VerifyHistoricalReviewAsync(args.ProductRoot, initial, token).ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is InvalidDataException or InvalidOperationException or NotSupportedException or JsonException or FileNotFoundException or DirectoryNotFoundException)
        {
            throw new ArtifactVerificationException();
        }
        var after = await store.ReadOneReadOnlyAsync(args.ResultId, token).ConfigureAwait(false);
        if (!SameRecord(initial, after)) throw new InvalidDataException("Review metadata changed during verification.");
        var job = artifacts.JobId.ToString("N");
        var transaction = artifacts.TransactionId.ToString("N");
        return new
        {
            targetFingerprint = Fingerprint(args.ProductRoot),
            resultId = initial.ResultId,
            transactionId = initial.TransactionId,
            reviewKind = initial.ReviewKind,
            artifactVerification = "verified",
            manifestSha256 = artifacts.ManifestSha256,
            output = new { relativePath = $"stitch-jobs/{job}/stitched.jpg", sha256 = artifacts.StitchedSha256, width = artifacts.Width, height = artifacts.Height },
            originals = artifacts.Originals.OrderBy(original => original.Alias).Select(original => new
            {
                alias = original.Alias,
                relativePath = $"transactions/{transaction}/{original.Alias}/original.jpg",
                sha256 = original.Sha256,
                width = original.Width,
                height = original.Height,
                size = original.SizeBytes,
            }).ToArray(),
            adapterSha256 = adapterHash,
            metadataUpdatedAtUtc = initial.UpdatedAtUtc,
            metadataObservedAtUtc = DateTimeOffset.UtcNow,
            hardwareAcceptance = "not evaluated",
        };
    }

    private static ReviewsArguments ParseReviews(string[] args)
    {
        if (args.Length < 3 || args[0] != "reviews") throw new ArgumentException("reviews requires root.");
        string? root = null;
        var offset = 0;
        var limit = 25;
        var offsetSeen = false;
        var limitSeen = false;
        for (var index = 1; index < args.Length; index += 2)
        {
            if (index + 1 >= args.Length) throw new ArgumentException("option value is missing.");
            var option = args[index];
            var value = args[index + 1];
            switch (option)
            {
                case "--root" when root is null:
                    if (!IsLocalWindowsDrivePath(value))
                        throw new ArgumentException("root must be absolute.");
                    root = Path.GetFullPath(value);
                    break;
                case "--offset" when !offsetSeen:
                    offset = ParseRange(value, 0, 1000);
                    offsetSeen = true;
                    break;
                case "--limit" when !limitSeen:
                    limit = ParseRange(value, 1, 25);
                    limitSeen = true;
                    break;
                default:
                    throw new ArgumentException("unknown or duplicate option.");
            }
        }
        if (root is null) throw new ArgumentException("root is required.");
        return new ReviewsArguments(root, offset, limit);
    }

    private static VerifyReviewArguments ParseVerifyReview(string[] args)
    {
        if (args.Length != 7 || args[0] != "verify-review") throw new ArgumentException("verify-review requires three options.");
        string? root = null;
        string? id = null;
        string? kind = null;
        for (var index = 1; index < args.Length; index += 2)
        {
            var option = args[index];
            var value = args[index + 1];
            switch (option)
            {
                case "--product-root" when root is null: root = NormalizeLocalWindowsPath(value); break;
                case "--result-id" when id is null && Guid.TryParseExact(value, "N", out var parsed) &&
                    parsed != Guid.Empty && value == parsed.ToString("N"): id = value; break;
                case "--expected-kind" when kind is null && value is "Product" or "Simulated": kind = value; break;
                default: throw new ArgumentException("unknown, duplicate, or invalid verify option.");
            }
        }
        return root is not null && id is not null && kind is not null
            ? new VerifyReviewArguments(root, id, kind) : throw new ArgumentException("verify-review options are incomplete.");
    }

    private static bool IsLocalWindowsDrivePath(string value)
    {
        if (string.IsNullOrWhiteSpace(value) || value.Length < 3 || !Path.IsPathFullyQualified(value) ||
            !char.IsAsciiLetter(value[0]) || value[1] != ':' || (value[2] != '\\' && value[2] != '/'))
            return false;
        if (value.StartsWith("\\\\", StringComparison.Ordinal) || value.StartsWith("//", StringComparison.Ordinal) ||
            value.StartsWith("\\\\?\\", StringComparison.Ordinal) || value.StartsWith("\\\\.\\", StringComparison.Ordinal))
            return false;
        try
        {
            return new DriveInfo(value[..3]).DriveType == DriveType.Fixed;
        }
        catch (ArgumentException)
        {
            return false;
        }
    }

    private static string NormalizeLocalWindowsPath(string value)
    {
        if (!IsLocalWindowsDrivePath(value) || value.IndexOf(':', 2) >= 0) throw new ArgumentException("path must be local.");
        return Path.TrimEndingDirectorySeparator(Path.GetFullPath(value));
    }

    private static void ValidateFixedLocalPath(string path, bool directory)
    {
        if (!IsLocalWindowsDrivePath(path) || path.IndexOf(':', 2) >= 0 || (directory ? !Directory.Exists(path) : !File.Exists(path)))
            throw new FileNotFoundException();
        for (FileSystemInfo? current = directory ? new DirectoryInfo(path) : new FileInfo(path); current is not null;
             current = current is DirectoryInfo folder ? folder.Parent : ((FileInfo)current).Directory)
        {
            if ((current.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("redirected path");
        }
    }

    private static string Fingerprint(string path) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(path.ToUpperInvariant())));
    private static bool SameRecord(OperatorReviewRecord left, OperatorReviewRecord right) =>
        left.ResultId == right.ResultId && left.TransactionId == right.TransactionId && left.ReviewKind == right.ReviewKind &&
        left.State == right.State && left.UpdatedAtUtc == right.UpdatedAtUtc;

    private static int ParseRange(string value, int minimum, int maximum)
    {
        if (!int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var parsed) ||
            parsed < minimum || parsed > maximum)
            throw new ArgumentException("numeric option is out of range.");
        return parsed;
    }

    private static int Fail(string requestId, string operation, string errorCode)
    {
        Write(new Envelope(requestId, ContractVersion, Build, "error", DateTimeOffset.UtcNow,
            operation, null, errorCode));
        return 2;
    }

    private static void Write(Envelope envelope) => Console.Out.WriteLine(JsonSerializer.Serialize(envelope, Json));
}
