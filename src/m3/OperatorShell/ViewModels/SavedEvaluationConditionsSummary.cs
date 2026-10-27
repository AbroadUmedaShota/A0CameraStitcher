using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace A0CameraStitcher.M3.OperatorShell.ViewModels;

// Display-only reader. It never opens referenced images/profile/jobs, runs a
// process, or converts a saved selection into an approved product request.
internal sealed record SavedEvaluationConditionsSummary(string ProfileName, string Description)
{
    internal static SavedEvaluationConditionsSummary Read(string path)
    {
        if (!Regex.IsMatch(path, @"\A[A-Za-z]:[\\/]", RegexOptions.CultureInvariant) || path[2..].Contains(':')) throw new InvalidDataException();
        RelativePath(path[3..].Replace('\\', '/'));
        path = Path.GetFullPath(path);
        if (new DriveInfo(Path.GetPathRoot(path)!).DriveType != DriveType.Fixed) throw new InvalidDataException();
        for (var parent = new FileInfo(path).Directory; parent is not null; parent = parent.Parent)
            if (!parent.Exists || (parent.Attributes & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException();
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException();
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (file.Length is <= 0 or > 64 * 1024) throw new InvalidDataException();
        var bytes = new byte[(int)file.Length];
        file.ReadExactly(bytes);
        return Parse(bytes);
    }

    internal static SavedEvaluationConditionsSummary Parse(byte[] bytes)
    {
        if (bytes.Length is 0 or > 64 * 1024) throw new InvalidDataException();
        try
        {
            using var document = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 8 });
            var root = document.RootElement;
            Fields(root, "schemaVersion", "productApproved", "quality", "pairId", "reviewReportSha256", "reviewImageSha256", "inputs", "profile", "correction", "color", "raster");
            if (Text(root, "schemaVersion") != "a0.saved-review-evaluation-conditions.v1" ||
                root.GetProperty("productApproved").ValueKind != JsonValueKind.False || Text(root, "quality") != "not-evaluated" ||
                string.IsNullOrEmpty(Text(root, "pairId"))) throw new InvalidDataException();
            Hash(root, "reviewReportSha256"); Hash(root, "reviewImageSha256");
            var inputs = root.GetProperty("inputs");
            if (inputs.ValueKind != JsonValueKind.Array || inputs.GetArrayLength() != 2) throw new InvalidDataException();
            for (var index = 0; index != 2; ++index)
            {
                var input = inputs[index];
                Fields(input, "cameraAlias", "sha256", "encodedSizeBytes");
                if (Text(input, "cameraAlias") != (index == 0 ? "CAM-A" : "CAM-B")) throw new InvalidDataException();
                Hash(input, "sha256"); PositiveSize(input, 64 * 1024 * 1024);
            }
            var profile = Reference(root.GetProperty("profile"), 256 * 1024);
            Reference(root.GetProperty("correction"), 64 * 1024);
            var color = root.GetProperty("color").ValueKind != JsonValueKind.Null;
            if (color) Reference(root.GetProperty("color"), 64 * 1024);
            var raster = root.GetProperty("raster");
            Fields(raster, "leftUm", "topUm", "rightUm", "bottomUm", "dpi", "widthPixels", "heightPixels", "resampling");
            var left = Integer(raster, "leftUm"); var top = Integer(raster, "topUm");
            var right = Integer(raster, "rightUm"); var bottom = Integer(raster, "bottomUm");
            var dpi = Integer(raster, "dpi", uint.MaxValue);
            var width = Integer(raster, "widthPixels", uint.MaxValue); var height = Integer(raster, "heightPixels", uint.MaxValue);
            if (right <= left || bottom <= top || dpi == 0 || width == 0 || height == 0 ||
                Text(raster, "resampling") is not ("bilinear" or "bicubic-catmull-rom") ||
                decimal.Ceiling(((decimal)right - left) * dpi / 25400) != width ||
                decimal.Ceiling(((decimal)bottom - top) * dpi / 25400) != height) throw new InvalidDataException();
            var text = string.Create(CultureInfo.InvariantCulture,
                $"出力範囲：左 {left / 1000m:0.###} / 上 {top / 1000m:0.###} ～ 右 {right / 1000m:0.###} / 下 {bottom / 1000m:0.###} mm\n出力密度：{dpi} DPI\n画像の大きさ：{width} × {height} 画素\n色補正：{(color ? "あり" : "なし")}");
            return new SavedEvaluationConditionsSummary(Path.GetFileName(profile), text);
        }
        catch (Exception error) when (error is JsonException or InvalidOperationException or FormatException or OverflowException)
        { throw new InvalidDataException("Invalid saved evaluation conditions.", error); }
    }

    private static void Fields(JsonElement element, params string[] names)
    {
        if (element.ValueKind != JsonValueKind.Object) throw new InvalidDataException();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var property in element.EnumerateObject())
            if (!names.Contains(property.Name, StringComparer.Ordinal) || !seen.Add(property.Name)) throw new InvalidDataException();
        if (seen.Count != names.Length) throw new InvalidDataException();
    }
    private static string Text(JsonElement element, string name) => element.GetProperty(name).GetString() ?? throw new InvalidDataException();
    private static void Hash(JsonElement element, string name)
    { if (!Regex.IsMatch(Text(element, name), @"\A[0-9a-f]{64}\z", RegexOptions.CultureInvariant)) throw new InvalidDataException(); }
    private static ulong Integer(JsonElement element, string name, ulong maximum = ulong.MaxValue)
    {
        var value = element.GetProperty(name);
        if (value.ValueKind != JsonValueKind.Number || !Regex.IsMatch(value.GetRawText(), "^(0|[1-9][0-9]*)$", RegexOptions.CultureInvariant) ||
            !value.TryGetUInt64(out var number) || number > maximum) throw new InvalidDataException();
        return number;
    }
    private static void PositiveSize(JsonElement element, ulong maximum)
    { if (Integer(element, "encodedSizeBytes", maximum) == 0) throw new InvalidDataException(); }
    private static string Reference(JsonElement element, ulong maximum)
    {
        Fields(element, "relativePath", "sha256", "encodedSizeBytes");
        Hash(element, "sha256"); PositiveSize(element, maximum);
        var path = Text(element, "relativePath");
        RelativePath(path);
        return path;
    }
    private static void RelativePath(string path)
    {
        if (path.Contains('\\')) throw new InvalidDataException();
        foreach (var part in path.Split('/'))
            if (string.IsNullOrEmpty(part) || part is "." or ".." || part.EndsWith('.') || part.EndsWith(' ') ||
                part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
                Regex.IsMatch(part, @"^(CON|PRN|AUX|NUL|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(?:\.|$)", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)) throw new InvalidDataException();
    }
}
