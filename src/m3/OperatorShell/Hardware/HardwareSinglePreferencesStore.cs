using System.IO;
using System.Text.Json;

namespace A0CameraStitcher.M3.OperatorShell.Hardware;

internal sealed record HardwareSinglePreferences(string ExportDirectory);

internal sealed class HardwareSinglePreferencesStore
{
    private const int MaximumBytes = 16 * 1024;
    private readonly string _path;

    public HardwareSinglePreferencesStore(string path)
    {
        _path = Path.GetFullPath(path);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(
            Path.GetDirectoryName(_path)!);
    }

    public async Task<HardwareSinglePreferences?> LoadAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            EnsureRegularLocalPreferencesFile(_path);
        }
        catch (Exception exception) when (
            exception is FileNotFoundException or DirectoryNotFoundException)
        {
            return null;
        }

        // Check the size on the same handle used to read/parse below, not on
        // a separate FileInfo query: a FileInfo.Length check followed by a
        // later, independent FileStream open is a TOCTOU window in which the
        // file on disk could be swapped for a larger one between the two
        // operations, defeating the size limit.
        await using var stream = new FileStream(
            _path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.Read,
            4096,
            FileOptions.Asynchronous | FileOptions.SequentialScan);
        if (stream.Length is <= 0 or > MaximumBytes)
        {
            throw new InvalidDataException("保存先設定ファイルのサイズが不正です。");
        }

        using var document = await JsonDocument.ParseAsync(
            stream,
            new JsonDocumentOptions { AllowTrailingCommas = false, CommentHandling = JsonCommentHandling.Disallow },
            cancellationToken).ConfigureAwait(false);
        var root = document.RootElement;
        if (root.ValueKind != JsonValueKind.Object || root.GetRawText().Length > MaximumBytes)
        {
            throw new InvalidDataException("保存先設定ファイルが不正です。");
        }

        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var property in root.EnumerateObject())
        {
            if (!names.Add(property.Name) || property.Name is not ("schemaVersion" or "exportDirectory"))
            {
                throw new InvalidDataException("保存先設定ファイルに重複または未知の項目があります。");
            }
        }

        if (names.Count != 2 ||
            root.GetProperty("schemaVersion").GetString() != "a0.hardware-single.preferences.v1")
        {
            throw new InvalidDataException("保存先設定ファイルのschemaが不正です。");
        }

        var exportDirectory = root.GetProperty("exportDirectory").GetString();
        if (string.IsNullOrWhiteSpace(exportDirectory) || !Path.IsPathFullyQualified(exportDirectory))
        {
            throw new InvalidDataException("保存先が絶対パスではありません。");
        }

        exportDirectory = Path.GetFullPath(exportDirectory);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(exportDirectory);
        return new HardwareSinglePreferences(exportDirectory);
    }

    private static void EnsureRegularLocalPreferencesFile(string path)
    {
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(path);
        if ((File.GetAttributes(path) & (FileAttributes.Directory | FileAttributes.ReparsePoint)) != 0)
        {
            throw new InvalidDataException("保存先設定ファイルは通常のローカルファイルである必要があります。");
        }
    }

    public async Task SaveAsync(string exportDirectory, CancellationToken cancellationToken = default)
    {
        var normalized = Path.GetFullPath(exportDirectory);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(normalized);
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        WindowsLocalPathGuard.EnsureExistingChainIsLocalAndNotReparse(Path.GetDirectoryName(_path)!);
        var bytes = JsonSerializer.SerializeToUtf8Bytes(new
        {
            schemaVersion = "a0.hardware-single.preferences.v1",
            exportDirectory = normalized,
        });
        await WriteDurablyAsync(bytes, cancellationToken).ConfigureAwait(false);
    }

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
