using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace A0CameraStitcher.M3.Foundation.DualCamera;

[JsonConverter(typeof(JsonStringEnumConverter<DualCameraIdentityStatus>))]
public enum DualCameraIdentityStatus
{
    Ready,
    Missing,
    Ambiguous,
    Collision,
    AliasMismatch,
    TransportMismatch,
    Expired,
    InvalidSchema,
    HardwarePending,
}

public sealed record DualCameraIdentitySnapshot(
    DualCameraIdentityStatus Status,
    string ReasonCode,
    DateTimeOffset ObservedAtUtc,
    DateTimeOffset ExpiresAtUtc)
{
    [JsonIgnore]
    public bool IsReady => Status == DualCameraIdentityStatus.Ready;

    public DualCameraIdentitySnapshot EvaluateAt(DateTimeOffset nowUtc) =>
        Status == DualCameraIdentityStatus.Ready && ExpiresAtUtc <= nowUtc
            ? this with
            {
                Status = DualCameraIdentityStatus.Expired,
                ReasonCode = "proof_stale",
            }
            : this;

    public static DualCameraIdentitySnapshot AnonymousTestSyntheticReady() => new(
        DualCameraIdentityStatus.Ready,
        "test_synthetic_anonymous_ready",
        DateTimeOffset.UnixEpoch,
        DateTimeOffset.MaxValue);

    public static DualCameraIdentitySnapshot HardwarePending() => new(
        DualCameraIdentityStatus.HardwarePending,
        "identity_strategy_unresolved",
        DateTimeOffset.UnixEpoch,
        DateTimeOffset.UnixEpoch);
}

public interface IDualCameraIdentitySnapshotSource
{
    event EventHandler<DualCameraIdentitySnapshot>? SnapshotChanged;

    DualCameraIdentitySnapshot Current { get; }
}

public sealed class FixedDualCameraIdentitySnapshotSource : IDualCameraIdentitySnapshotSource
{
    public FixedDualCameraIdentitySnapshotSource(DualCameraIdentitySnapshot snapshot)
    {
        Current = snapshot ?? throw new ArgumentNullException(nameof(snapshot));
    }

    public event EventHandler<DualCameraIdentitySnapshot>? SnapshotChanged
    {
        add { }
        remove { }
    }

    public DualCameraIdentitySnapshot Current { get; }
}

public static class DualCameraNativeIdentityAdapter
{
    public const string SchemaVersion = "a0.dual-identity-application.v1";

    public static DualCameraIdentitySnapshot ParseAnonymousSnapshot(
        string json,
        DateTimeOffset nowUtc)
    {
        if (string.IsNullOrWhiteSpace(json) || nowUtc.Offset != TimeSpan.Zero)
        {
            return InvalidSchema();
        }

        try
        {
            using var document = JsonDocument.Parse(json, new JsonDocumentOptions
            {
                AllowTrailingCommas = false,
                CommentHandling = JsonCommentHandling.Disallow,
                MaxDepth = 8,
            });
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !HasExactly(root, "schemaVersion", "reason", "observedAtUtc", "expiresAtUtc", "bindings") ||
                !TryString(root, "schemaVersion", out var schema) ||
                !string.Equals(schema, SchemaVersion, StringComparison.Ordinal) ||
                !TryString(root, "reason", out var reason) ||
                !TryUtc(root, "observedAtUtc", out var observedAtUtc) ||
                !TryUtc(root, "expiresAtUtc", out var expiresAtUtc) ||
                !root.TryGetProperty("bindings", out var bindings) ||
                bindings.ValueKind != JsonValueKind.Array)
            {
                return InvalidSchema();
            }

            var mapped = MapNativeReason(reason);
            if (!IsKnownReason(reason))
            {
                return InvalidSchema();
            }
            if (mapped != DualCameraIdentityStatus.Ready)
            {
                return new(mapped, reason, observedAtUtc, expiresAtUtc);
            }
            if (observedAtUtc > nowUtc)
            {
                return InvalidSchema();
            }
            if (expiresAtUtc <= nowUtc || expiresAtUtc <= observedAtUtc)
            {
                return new(DualCameraIdentityStatus.Expired, "proof_stale", observedAtUtc, expiresAtUtc);
            }

            return ValidateReadyBindings(bindings, observedAtUtc, expiresAtUtc);
        }
        catch (JsonException)
        {
            return InvalidSchema();
        }
    }

    private static DualCameraIdentitySnapshot ValidateReadyBindings(
        JsonElement bindings,
        DateTimeOffset observedAtUtc,
        DateTimeOffset expiresAtUtc)
    {
        if (bindings.GetArrayLength() != 2)
        {
            return new(DualCameraIdentityStatus.Missing, "camera_count_mismatch", observedAtUtc, expiresAtUtc);
        }

        var aliases = new HashSet<string>(StringComparer.Ordinal);
        var sdkDigests = new HashSet<string>(StringComparer.Ordinal);
        var wpdDigests = new HashSet<string>(StringComparer.Ordinal);
        foreach (var binding in bindings.EnumerateArray())
        {
            if (binding.ValueKind != JsonValueKind.Object ||
                !HasExactly(binding, "alias", "sdkIdentitySha256", "wpdIdentitySha256") ||
                !TryString(binding, "alias", out var alias) ||
                !TryString(binding, "sdkIdentitySha256", out var sdkDigest, allowEmpty: true) ||
                !TryString(binding, "wpdIdentitySha256", out var wpdDigest, allowEmpty: true))
            {
                return InvalidSchema();
            }
            if (alias is not ("CAM-A" or "CAM-B") || !aliases.Add(alias))
            {
                return new(DualCameraIdentityStatus.AliasMismatch, "alias_cardinality_mismatch", observedAtUtc, expiresAtUtc);
            }
            if (string.IsNullOrEmpty(sdkDigest) || string.IsNullOrEmpty(wpdDigest))
            {
                return new(DualCameraIdentityStatus.Missing, "missing_identity", observedAtUtc, expiresAtUtc);
            }
            if (!IsAnonymousDigest(sdkDigest) || !IsAnonymousDigest(wpdDigest))
            {
                return InvalidSchema();
            }
            if (!sdkDigests.Add(sdkDigest) || !wpdDigests.Add(wpdDigest))
            {
                return new(DualCameraIdentityStatus.Collision, "duplicate_identity", observedAtUtc, expiresAtUtc);
            }
        }
        if (!aliases.SetEquals(["CAM-A", "CAM-B"]))
        {
            return new(DualCameraIdentityStatus.AliasMismatch, "alias_cardinality_mismatch", observedAtUtc, expiresAtUtc);
        }
        if (sdkDigests.Overlaps(wpdDigests))
        {
            return new(DualCameraIdentityStatus.TransportMismatch, "mismatched_transport", observedAtUtc, expiresAtUtc);
        }
        return new(DualCameraIdentityStatus.Ready, "ready", observedAtUtc, expiresAtUtc);
    }

    private static DualCameraIdentityStatus MapNativeReason(string reason) => reason switch
    {
        "ready" => DualCameraIdentityStatus.Ready,
        "proof_count_mismatch" or "camera_count_mismatch" or "missing_identity" => DualCameraIdentityStatus.Missing,
        "confirmation_mismatch" or "unbound_identity" => DualCameraIdentityStatus.Ambiguous,
        "duplicate_identity" or "identity_collision" => DualCameraIdentityStatus.Collision,
        "alias_cardinality_mismatch" => DualCameraIdentityStatus.AliasMismatch,
        "mismatched_transport" => DualCameraIdentityStatus.TransportMismatch,
        "proof_stale" => DualCameraIdentityStatus.Expired,
        "proof_invalid" or "proof_tampered" or "provider_mismatch" => DualCameraIdentityStatus.InvalidSchema,
        "identity_strategy_unresolved" or "legacy_map_fallback_prohibited" => DualCameraIdentityStatus.HardwarePending,
        _ => DualCameraIdentityStatus.InvalidSchema,
    };

    private static bool IsKnownReason(string reason) => reason is
        "ready" or
        "proof_count_mismatch" or "camera_count_mismatch" or "missing_identity" or
        "confirmation_mismatch" or "unbound_identity" or
        "duplicate_identity" or "identity_collision" or
        "alias_cardinality_mismatch" or "mismatched_transport" or "proof_stale" or
        "proof_invalid" or "proof_tampered" or "provider_mismatch" or
        "identity_strategy_unresolved" or "legacy_map_fallback_prohibited";

    private static bool HasExactly(JsonElement element, params string[] names)
    {
        var expected = new HashSet<string>(names, StringComparer.Ordinal);
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var property in element.EnumerateObject())
        {
            if (!expected.Contains(property.Name) || !seen.Add(property.Name))
            {
                return false;
            }
        }
        return seen.SetEquals(expected);
    }

    private static bool TryString(
        JsonElement element,
        string name,
        out string value,
        bool allowEmpty = false)
    {
        value = string.Empty;
        if (!element.TryGetProperty(name, out var property) || property.ValueKind != JsonValueKind.String)
        {
            return false;
        }
        value = property.GetString() ?? string.Empty;
        return allowEmpty || value.Length > 0;
    }

    private static bool TryUtc(JsonElement element, string name, out DateTimeOffset value)
    {
        value = default;
        return TryString(element, name, out var text) &&
            DateTimeOffset.TryParse(text, CultureInfo.InvariantCulture, DateTimeStyles.None, out value) &&
            value.Offset == TimeSpan.Zero &&
            text.EndsWith('Z');
    }

    private static bool IsAnonymousDigest(string value) =>
        value.Length == 64 && value.All(character => character is >= '0' and <= '9' or >= 'a' and <= 'f');

    private static DualCameraIdentitySnapshot InvalidSchema() => new(
        DualCameraIdentityStatus.InvalidSchema,
        "invalid_schema",
        DateTimeOffset.UnixEpoch,
        DateTimeOffset.UnixEpoch);
}
