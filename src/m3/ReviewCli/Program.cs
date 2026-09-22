using System.Globalization;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation;

namespace A0CameraStitcher.M3.ReviewCli;

internal static class Program
{
    private const string AppId = "a0-camera-stitcher-review-cli";
    private const int ContractVersion = 1;
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
        public string Environment => "Windows-local standalone metadata-only";
    }

    private sealed record ReviewsArguments(string Root, int Offset, int Limit);

    public static async Task<int> Main(string[] args)
    {
        var requestId = Guid.NewGuid().ToString("N");
        var operation = args.FirstOrDefault() is "describe" or "reviews" ? args[0] : "unknown";
        try
        {
            object data = operation switch
            {
                "describe" => Describe(args),
                "reviews" => await ReviewsAsync(ParseReviews(args), CancellationToken.None),
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
            environment = "Windows-local standalone metadata-only",
            operations = new[] { "describe", "reviews" },
            mutation = "not supported",
            guiInstance = "not used",
            artifactVerification = "not performed",
            limits = new { maxScanned = 1000, offset = "0..1000", limit = "1..25", defaultOffset = 0, defaultLimit = 25 },
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
