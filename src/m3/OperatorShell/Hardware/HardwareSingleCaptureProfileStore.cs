using System.Globalization;
using System.IO;
using System.Text.Json;
using A0CameraStitcher.M3.Foundation.Hardware;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal sealed class HardwareSingleCaptureProfileStore
{
    private static readonly TimeSpan Validity = TimeSpan.FromDays(30);
    private readonly string _path;
    private readonly TimeProvider _timeProvider;

    public HardwareSingleCaptureProfileStore(string path, TimeProvider? timeProvider = null)
    {
        _path = Path.GetFullPath(path);
        _timeProvider = timeProvider ?? TimeProvider.System;
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(
            Path.GetDirectoryName(_path)!);
    }

    public string ProfilePath => _path;

    public async Task ApproveCamAAsync(
        HardwareObservedCameraSettings observed,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(observed);
        ValidateCriticalSettings(observed);
        var approvedAt = _timeProvider.GetUtcNow();
        var expiresAt = approvedAt.Add(Validity);
        var settings = new Dictionary<string, object?>(StringComparer.Ordinal)
        {
            ["fileType"] = ExpectUnavailable(observed.FileType, "FileType"),
            ["compressionLevel"] = ExpectLabel(observed.CompressionLevel, "JPEG Fine", "CompressionLevel"),
            ["imageSize"] = ExpectLabel(observed.ImageSize, "L(7360*4912)", "ImageSize"),
            ["exposureMode"] = ExpectUnsignedValue(observed.ExposureMode, 3, "ExposureMode(S)"),
            ["shutterSpeed"] = ExpectLabel(observed.ShutterSpeed, "1/6", "ShutterSpeed"),
            ["aperture"] = ExpectLabel(observed.Aperture, "8", "Aperture(F8)"),
            ["sensitivity"] = ExpectLabel(observed.Sensitivity, "64", "Sensitivity(ISO64)"),
            ["whiteBalanceMode"] = ExpectLabel(observed.WhiteBalanceMode, "Preset 1", "WhiteBalanceMode"),
            ["focusMode"] = ExpectOpaqueFocus(observed.FocusMode),
        };
        var profileId = $"single-cam-a-{approvedAt:yyyyMMdd}";
        var payload = new
        {
            schemaVersion = "a0.camera-agent.capture-profile.v1",
            profileId,
            profileVersion = 1,
            selectedAlias = "CAM-A",
            cameraMode = "SingleCamera",
            approved = true,
            approvedBy = "local-operator",
            approvalReference = $"app:{Guid.NewGuid():N}",
            approvedAtUtc = CanonicalUtc(approvedAt),
            expiresAtUtc = CanonicalUtc(expiresAt),
            expectedSettings = settings,
        };

        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(Path.GetDirectoryName(_path)!);
        var bytes = JsonSerializer.SerializeToUtf8Bytes(payload);
        await WriteDurablyAsync(bytes, cancellationToken).ConfigureAwait(false);
    }

    private static Dictionary<string, object?> ExpectUnavailable(
        HardwareObservedCameraSetting setting,
        string name)
    {
        if (setting.Available)
        {
            throw new InvalidOperationException($"{name}は未提供であることが承認条件です。");
        }
        return new Dictionary<string, object?> { ["available"] = false };
    }

    private static Dictionary<string, object?> ExpectLabel(
        HardwareObservedCameraSetting setting,
        string expected,
        string name)
    {
        if (!setting.Available || !string.Equals(setting.CurrentLabel, expected, StringComparison.Ordinal))
        {
            throw new InvalidOperationException($"{name}は{expected}でなければ承認できません。");
        }
        return ExactObservation(setting);
    }

    private static Dictionary<string, object?> ExpectUnsignedValue(
        HardwareObservedCameraSetting setting,
        uint expected,
        string name)
    {
        if (!setting.Available || setting.CurrentValue != expected)
        {
            throw new InvalidOperationException($"{name}のread-only値が{expected}でなければ承認できません。");
        }
        return ExactObservation(setting);
    }

    private static Dictionary<string, object?> ExpectOpaqueFocus(HardwareObservedCameraSetting setting)
    {
        if (!setting.Available ||
            (!setting.CurrentValue.HasValue && !setting.CurrentIndex.HasValue &&
             string.IsNullOrWhiteSpace(setting.CurrentLabel)))
        {
            throw new InvalidOperationException("FocusModeのread-only tokenを取得できません。");
        }
        return ExactObservation(setting);
    }

    private static Dictionary<string, object?> ExactObservation(HardwareObservedCameraSetting setting)
    {
        var result = new Dictionary<string, object?>
        {
            ["available"] = setting.Available,
            ["capType"] = setting.CapType,
            ["probeState"] = setting.ProbeState,
            ["valueType"] = setting.ValueType,
        };
        if (setting.CurrentValue.HasValue) result["currentValue"] = setting.CurrentValue.Value;
        if (setting.CurrentIndex.HasValue) result["currentIndex"] = setting.CurrentIndex.Value;
        if (setting.CurrentLabel is not null) result["currentLabel"] = setting.CurrentLabel;
        return result;
    }

    private static void ValidateCriticalSettings(HardwareObservedCameraSettings observed)
    {
        _ = ExpectUnavailable(observed.FileType, "FileType");
        _ = ExpectLabel(observed.CompressionLevel, "JPEG Fine", "CompressionLevel");
        _ = ExpectLabel(observed.ImageSize, "L(7360*4912)", "ImageSize");
        _ = ExpectUnsignedValue(observed.ExposureMode, 3, "ExposureMode(S)");
        _ = ExpectLabel(observed.ShutterSpeed, "1/6", "ShutterSpeed");
        _ = ExpectLabel(observed.Aperture, "8", "Aperture(F8)");
        _ = ExpectLabel(observed.Sensitivity, "64", "Sensitivity(ISO64)");
        _ = ExpectLabel(observed.WhiteBalanceMode, "Preset 1", "WhiteBalanceMode");
        _ = ExpectOpaqueFocus(observed.FocusMode);
    }

    private static string CanonicalUtc(DateTimeOffset value) =>
        value.ToUniversalTime().ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);

    private async Task WriteDurablyAsync(byte[] bytes, CancellationToken cancellationToken)
    {
        var partial = $"{_path}.{Guid.NewGuid():N}.partial";
        try
        {
            await using (var stream = new FileStream(
                             partial,
                             FileMode.CreateNew,
                             FileAccess.Write,
                             FileShare.None,
                             4096,
                             FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await stream.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
                await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
                stream.Flush(flushToDisk: true);
            }
            WindowsDurableFilePublisher.Publish(partial, _path, replaceExisting: true);
        }
        catch
        {
            if (File.Exists(partial)) File.Delete(partial);
            throw;
        }
    }
}
