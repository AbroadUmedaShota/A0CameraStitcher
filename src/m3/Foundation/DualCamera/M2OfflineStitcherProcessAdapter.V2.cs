using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace A0CameraStitcher.M3.Foundation.DualCamera;

public enum V2StitchResampling { Bilinear, BicubicCatmullRom }

public sealed partial class M2OfflineStitcherProcessAdapter
{
    private const long V2MaximumJpegBytes = 64L * 1024 * 1024;
    private const long V2MaximumJsonBytes = 256 * 1024;
    private const string V2ManifestName = "stitch-job.manifest.json";
    private static readonly UTF8Encoding V2Utf8 = new(false, true);

    /// <summary>Explicit approved-v2 offline stitching. No profile selection or camera control occurs here.</summary>
    public async Task<OfflineStitchArtifact> StitchV2Async(
        IReadOnlyList<CanonicalJpegOriginal> originals, string outputJobDirectory, string approvedProfilePath,
        RigProfileV2 expectedProfile, DateTimeOffset assessedAtUtc, V2StitchResampling resampling,
        Guid stitchJobId, Guid captureTransactionId, DateTimeOffset completedAtUtc, CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(originals); ArgumentNullException.ThrowIfNull(expectedProfile);
        cancellationToken.ThrowIfCancellationRequested();
        V2Require(stitchJobId != Guid.Empty && captureTransactionId != Guid.Empty, "Non-empty job and capture IDs are required.");
        var assessed = V2Utc(assessedAtUtc); var completed = V2Utc(completedAtUtc);
        V2Require(completedAtUtc >= assessedAtUtc, "Completion cannot precede assessment.");
        expectedProfile.ValidateApprovedForUse(assessedAtUtc);
        string kernel = resampling switch { V2StitchResampling.Bilinear => "bilinear", V2StitchResampling.BicubicCatmullRom => "bicubic-catmull-rom", _ => throw new InvalidDataException("Unknown interpolation kernel.") };
        var pair = originals.ToArray();
        V2Require(pair.Length == 2 && pair[0].Alias == "CAM-A" && pair[1].Alias == "CAM-B", "Exactly CAM-A then CAM-B originals are required.");
        string directory = V2LocalPath(outputJobDirectory, requireFile: false);
        V2Require(!Directory.Exists(directory) && !File.Exists(directory), "StitchJob directory already exists.");
        V2Require(Directory.Exists(Path.GetDirectoryName(directory)), "StitchJob parent must already exist.");
        string profilePath = V2LocalPath(approvedProfilePath, requireFile: true);
        string cameraAPath = V2OriginalPath(pair[0]), cameraBPath = V2OriginalPath(pair[1]);
        // Named ancestors stay regular and cannot be replaced while the native
        // process reopens paths. Child-file creation is independent of these
        // directory metadata handles; no write/delete access is requested.
        using var directoryLocks = new V2DirectoryLocks(Path.GetDirectoryName(directory)!,
            Path.GetDirectoryName(profilePath)!, Path.GetDirectoryName(cameraAPath)!, Path.GetDirectoryName(cameraBPath)!);
        using var profileFile = V2OpenLocked(profilePath, V2MaximumJsonBytes);
        var actualProfile = RigProfileV2.Parse(V2Decode(await V2ReadLockedAsync(profileFile, V2MaximumJsonBytes, cancellationToken).ConfigureAwait(false)));
        actualProfile.ValidateApprovedForUse(assessedAtUtc);
        V2Require(actualProfile.FingerprintSha256 == expectedProfile.FingerprintSha256, "Profile file differs from the immutable expected snapshot.");
        using var a = V2OpenLocked(cameraAPath, V2MaximumJpegBytes);
        using var b = V2OpenLocked(cameraBPath, V2MaximumJpegBytes);
        V2Require(!V2SameFile(a.SafeFileHandle,b.SafeFileHandle), "Originals must be distinct files.");
        await V2VerifyOriginalAsync(a, pair[0], cancellationToken).ConfigureAwait(false);
        await V2VerifyOriginalAsync(b, pair[1], cancellationToken).ConfigureAwait(false);
        var stdout = await RunAsync([
            "stitch-v2", "--camera-a", cameraAPath, "--camera-b", cameraBPath, "--job-directory", directory,
            "--profile-file", profilePath, "--expected-profile-sha256", expectedProfile.FingerprintSha256,
            "--assessed-at", assessed, "--resampling", kernel, "--stitch-job-id", stitchJobId.ToString("N"),
            "--capture-transaction-id", captureTransactionId.ToString("N"), "--completed-at", completed
        ], cancellationToken).ConfigureAwait(false);
        var raster = expectedProfile.OutputRaster!;
        V2CheckStitchResponse(stdout, expectedProfile, raster, stitchJobId);
        using var publishedDirectoryLocks = new V2DirectoryLocks(directory);
        string outputPath = V2LocalPath(Path.Combine(directory, "stitched.jpg"), requireFile: true);
        string manifestPath = V2LocalPath(Path.Combine(directory, V2ManifestName), requireFile: true);
        using var output = V2OpenLocked(outputPath, V2MaximumJpegBytes);
        using var manifest = V2OpenLocked(manifestPath, V2MaximumJsonBytes);
        var manifestBytes = await V2ReadLockedAsync(manifest, V2MaximumJsonBytes, cancellationToken).ConfigureAwait(false);
        var outputBytes = await V2ReadLockedAsync(output, V2MaximumJpegBytes, cancellationToken).ConfigureAwait(false);
        string outputHash = Convert.ToHexStringLower(SHA256.HashData(outputBytes));
        V2CheckManifest(V2Decode(manifestBytes), expectedProfile, kernel, pair, stitchJobId, captureTransactionId,
            completed, outputHash, outputBytes.LongLength);
        V2CheckJpeg(outputBytes, raster.WidthPixels, raster.HeightPixels);
        V2CheckJfifAndExif(outputBytes, raster.Dpi);
        // Native complete decode and manifest verification observe the same read-locked leaves.
        string verification = await RunAsync(["verify-published-stitch", "--job-directory", directory,
            "--stitch-job-id", stitchJobId.ToString("N"), "--capture-transaction-id", captureTransactionId.ToString("N")], cancellationToken).ConfigureAwait(false);
        var verifyLines = verification.Split(['\r','\n'], StringSplitOptions.RemoveEmptyEntries);
        V2Require(verifyLines.Length == 2 && verifyLines[0] == "result=verified-published-stitch" && verifyLines[1].StartsWith("manifestJson=", StringComparison.Ordinal), "Invalid native verification response.");
        V2CheckManifest(verifyLines[1]["manifestJson=".Length..], expectedProfile, kernel, pair, stitchJobId,
            captureTransactionId, completed, outputHash, outputBytes.LongLength);
        await ValidateCanonicalJpegAsync(outputPath, raster.WidthPixels, raster.HeightPixels, cancellationToken).ConfigureAwait(false);
        await V2VerifyOriginalAsync(a, pair[0], cancellationToken).ConfigureAwait(false);
        await V2VerifyOriginalAsync(b, pair[1], cancellationToken).ConfigureAwait(false);
        cancellationToken.ThrowIfCancellationRequested();
        return new OfflineStitchArtifact(outputPath, raster.WidthPixels, raster.HeightPixels, expectedProfile.ProfileId!, V2ManifestName);
    }

    private static void V2Require(bool condition, string message) { if (!condition) throw new InvalidDataException(message); }
    private static string V2Utc(DateTimeOffset value)
    {
        V2Require(value.Offset == TimeSpan.Zero && value.Ticks % TimeSpan.TicksPerSecond == 0, "UTC arguments require whole seconds.");
        return value.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);
    }
    private static string V2Decode(byte[] bytes)
    { try { return V2Utf8.GetString(bytes); } catch (DecoderFallbackException error) { throw new InvalidDataException("Invalid UTF-8 artifact.", error); } }
    private static string V2OriginalPath(CanonicalJpegOriginal original)
    {
        V2Require(original.IsCanonicalJpeg && original.Width == 7360 && original.Height == 4912 &&
            original.SizeBytes is > 0 and <= V2MaximumJpegBytes && V2Sha(original.Sha256), "Invalid D810 original metadata.");
        string path = V2LocalPath(original.Path, requireFile: true);
        V2Require(Path.GetFileName(path) == "original.jpg", "Canonical originals must be named original.jpg."); return path;
    }
    private static string V2LocalPath(string path, bool requireFile)
    {
        V2Require(!string.IsNullOrWhiteSpace(path) && Path.IsPathFullyQualified(path) && !path.StartsWith("\\\\", StringComparison.Ordinal) && !path.StartsWith("//", StringComparison.Ordinal), "A fixed local Windows path is required.");
        string? suppliedRoot = Path.GetPathRoot(path);
        V2Require(suppliedRoot is { Length: 3 } && suppliedRoot[1] == ':', "Device and UNC paths are forbidden.");
        foreach (string segment in path[suppliedRoot!.Length..].Split(['\\','/'], StringSplitOptions.RemoveEmptyEntries))
        {
            string device = segment.Split('.')[0].TrimEnd(' ','.');
            bool reserved = device.Equals("CON",StringComparison.OrdinalIgnoreCase) || device.Equals("PRN",StringComparison.OrdinalIgnoreCase) || device.Equals("AUX",StringComparison.OrdinalIgnoreCase) || device.Equals("NUL",StringComparison.OrdinalIgnoreCase) ||
                (device.Length == 4 && (device.StartsWith("COM",StringComparison.OrdinalIgnoreCase) || device.StartsWith("LPT",StringComparison.OrdinalIgnoreCase)) && "123456789¹²³".Contains(device[3]));
            V2Require(segment is not ("." or "..") && !segment.EndsWith('.') && !segment.EndsWith(' ') &&
                segment.IndexOfAny(Path.GetInvalidFileNameChars()) < 0 && !reserved, "Path includes a non-regular Windows component.");
        }
        string full = Path.GetFullPath(path); string root = Path.GetPathRoot(full)!;
        V2Require(new DriveInfo(root).DriveType == DriveType.Fixed, "Path must use a fixed local drive.");
        var parent = requireFile || !Directory.Exists(full) ? Path.GetDirectoryName(full)! : full;
        for (DirectoryInfo? current = new(parent); current is not null; current = current.Parent)
            V2Require(current.Exists && (current.Attributes & FileAttributes.ReparsePoint) == 0, "Path ancestor is missing or redirected.");
        if (File.Exists(full) || Directory.Exists(full)) V2Require((File.GetAttributes(full) & FileAttributes.ReparsePoint) == 0, "Artifact is redirected.");
        if (requireFile) V2Require(File.Exists(full), "Required regular file is unavailable."); return full;
    }
    private static FileStream V2OpenLocked(string path, long maximum)
    {
        var handle = V2CreateFile(path,0x80000000,1,IntPtr.Zero,3,0x00200000|0x08000000,IntPtr.Zero);
        if(handle.IsInvalid) { handle.Dispose(); throw new IOException("Artifact cannot be locked for immutable read."); }
        FileStream stream;
        try { var info=V2Information(handle); V2Require((info.Attributes & (0x10u|0x400u))==0,"Locked artifact is not a regular file.");
            stream=new FileStream(handle,FileAccess.Read,81920,isAsync:false); }
        catch { handle.Dispose(); throw; }
        if (stream.Length is <= 0 || stream.Length > maximum) { stream.Dispose(); throw new InvalidDataException("Artifact exceeds its resource bound."); }
        return stream;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct V2FileInformation
    {
        public uint Attributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME Created,Accessed,Written;
        public uint Volume,SizeHigh,SizeLow,Links,IndexHigh,IndexLow;
    }
    [DllImport("kernel32.dll",EntryPoint="CreateFileW",CharSet=CharSet.Unicode,SetLastError=true)]
    private static extern SafeFileHandle V2CreateFile(string path,uint access,uint sharing,IntPtr security,uint creation,uint flags,IntPtr template);
    [DllImport("kernel32.dll",EntryPoint="GetFileInformationByHandle",SetLastError=true)]
    [return:MarshalAs(UnmanagedType.Bool)]
    private static extern bool V2GetInformation(SafeFileHandle handle,out V2FileInformation information);
    private static V2FileInformation V2Information(SafeFileHandle handle)
    { if(!V2GetInformation(handle,out var info)) throw new IOException("Locked artifact metadata is unavailable."); return info; }
    private static bool V2SameFile(SafeFileHandle a,SafeFileHandle b)
    { var left=V2Information(a); var right=V2Information(b); return left.Volume==right.Volume && left.IndexHigh==right.IndexHigh && left.IndexLow==right.IndexLow; }
    private sealed class V2DirectoryLocks : IDisposable
    {
        private readonly List<SafeFileHandle> handles=[];
        internal V2DirectoryLocks(params string[] directories)
        {
            var seen=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            try { foreach(string directory in directories) for(DirectoryInfo? item=new(directory);item is not null;item=item.Parent)
                if(seen.Add(item.FullName)) {
                    var handle=V2CreateFile(item.FullName,0x80,1,IntPtr.Zero,3,0x02000000|0x00200000,IntPtr.Zero);
                    if(handle.IsInvalid) { handle.Dispose(); throw new IOException("Artifact directory cannot be locked."); }
                    handles.Add(handle); var info=V2Information(handle);
                    V2Require((info.Attributes&0x10)!=0 && (info.Attributes&0x400)==0,"Artifact directory is redirected or not regular.");
                }
            }
            catch { Dispose(); throw; }
        }
        public void Dispose() { foreach(var handle in handles) handle.Dispose(); handles.Clear(); }
    }
    private static async Task<byte[]> V2ReadLockedAsync(FileStream stream, long maximum, CancellationToken token)
    {
        V2Require(stream.Length is > 0 && stream.Length <= maximum, "Locked artifact exceeds its resource bound.");
        stream.Position = 0; var bytes = new byte[checked((int)stream.Length)];
        await stream.ReadExactlyAsync(bytes.AsMemory(), token).ConfigureAwait(false);
        V2Require(stream.Length == bytes.LongLength, "Locked artifact changed size."); return bytes;
    }
    private static async Task V2VerifyOriginalAsync(FileStream stream, CanonicalJpegOriginal original, CancellationToken token)
    {
        var bytes = await V2ReadLockedAsync(stream, V2MaximumJpegBytes, token).ConfigureAwait(false);
        V2Require(bytes.LongLength == original.SizeBytes && Convert.ToHexStringLower(SHA256.HashData(bytes)) == original.Sha256,
            "Original differs from expected capture metadata.");
        V2CheckJpeg(bytes, original.Width, original.Height);
    }
    private static void V2CheckJpeg(byte[] bytes, int width, int height)
    {
        V2Require(bytes.Length >= 4 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[^2] == 0xff && bytes[^1] == 0xd9, "JPEG envelope is invalid.");
        var dimensions = DualCameraProductFlow.ReadJpegDimensions(bytes);
        V2Require(dimensions.Width == width && dimensions.Height == height, "JPEG dimensions do not match the request.");
    }
    private static void V2CheckStitchResponse(string stdout, RigProfileV2 profile, RigOutputRasterV2 raster, Guid job)
    {
        var lines = stdout.Split(['\r','\n'], StringSplitOptions.RemoveEmptyEntries);
        var values = new Dictionary<string,string>(StringComparer.Ordinal);
        foreach (string line in lines) { int equal = line.IndexOf('='); V2Require(equal > 0 && values.TryAdd(line[..equal], line[(equal+1)..]), "Malformed or duplicated stitch response."); }
        V2Require(values.Count == 6 && values.GetValueOrDefault("result") == "stitched" && values.GetValueOrDefault("width") == raster.WidthPixels.ToString(CultureInfo.InvariantCulture) &&
            values.GetValueOrDefault("height") == raster.HeightPixels.ToString(CultureInfo.InvariantCulture) && values.GetValueOrDefault("profileId") == profile.ProfileId &&
            values.GetValueOrDefault("stitchJobId") == job.ToString("N") && values.GetValueOrDefault("manifest") == V2ManifestName,
            "Native stitch response does not match the requested profile, job or output.");
    }
    private static bool V2Sha(string? hash) => hash is { Length:64 } && hash.All(ch => ch is >= '0' and <= '9' or >= 'a' and <= 'f');
    private static void V2Fields(JsonElement node, params string[] fields)
    {
        V2Require(node.ValueKind == JsonValueKind.Object, "Expected manifest object.");
        var names = node.EnumerateObject().Select(property=>property.Name).ToArray();
        V2Require(names.Length == fields.Length && names.Distinct(StringComparer.Ordinal).Count() == fields.Length && names.All(name=>fields.Contains(name,StringComparer.Ordinal)), "Missing, duplicated or unknown manifest fields.");
    }
    private static string V2String(JsonElement node, string name)
    { var value = node.GetProperty(name); V2Require(value.ValueKind == JsonValueKind.String, "Expected manifest string."); return value.GetString()!; }
    private static long V2Integer(JsonElement node, string name, long maximum, bool zero = false)
    {
        var value=node.GetProperty(name); V2Require(value.ValueKind==JsonValueKind.Number,"Expected manifest integer."); string raw=value.GetRawText();
        V2Require(raw.Length>0 && raw.All(ch=>ch is >= '0' and <= '9') && long.TryParse(raw,NumberStyles.None,CultureInfo.InvariantCulture,out _),"Manifest integer has invalid lexical form.");
        long number=long.Parse(raw,NumberStyles.None,CultureInfo.InvariantCulture); V2Require(number >= (zero ? 0 : 1) && number <= maximum,"Manifest integer is outside its bounds."); return number;
    }
    private static JsonDocument V2ManifestDocument(string json)
    {
        int byteCount=V2Utf8.GetByteCount(json);
        V2Require(byteCount>0 && byteCount<=V2MaximumJsonBytes,"Manifest exceeds its resource bound.");
        try { return JsonDocument.Parse(json,new JsonDocumentOptions { MaxDepth=32, AllowTrailingCommas=false, CommentHandling=JsonCommentHandling.Disallow }); }
        catch(JsonException error) { throw new InvalidDataException("Malformed manifest JSON.",error); }
    }
    private static void V2CheckManifest(string json, RigProfileV2 profile, string kernel, CanonicalJpegOriginal[] originals,
        Guid job, Guid capture, string completed, string outputHash, long outputSize)
    {
        using var document=V2ManifestDocument(json); var root=document.RootElement;
        V2Fields(root,"schemaVersion","stitchJobId","captureTransactionId","inputs","rigProfile","engine","output","seamNavigation","terminalResultState","completedAtUtc","automaticRetryCount");
        V2Require(V2String(root,"schemaVersion")=="a0.stitch-job-manifest.v2" && V2String(root,"stitchJobId")==job.ToString("N") &&
            V2String(root,"captureTransactionId")==capture.ToString("N") && V2String(root,"terminalResultState")=="Succeeded" &&
            V2String(root,"completedAtUtc")==completed && V2Integer(root,"automaticRetryCount",0,true)==0,"Manifest identity, time or result state mismatch.");
        var inputs=root.GetProperty("inputs"); V2Require(inputs.ValueKind==JsonValueKind.Array && inputs.GetArrayLength()==2,"Manifest must bind two ordered inputs.");
        for(int index=0;index<2;index++) { var input=inputs[index]; V2Fields(input,"cameraAlias","sha256","encodedSizeBytes");
            V2Require(V2String(input,"cameraAlias")==originals[index].Alias && V2String(input,"sha256")==originals[index].Sha256 &&
                V2Integer(input,"encodedSizeBytes",V2MaximumJpegBytes)==originals[index].SizeBytes,"Manifest original binding mismatch."); }
        var rig=root.GetProperty("rigProfile"); V2Fields(rig,"profileId","version","sha256");
        V2Require(V2String(rig,"profileId")==profile.ProfileId && V2String(rig,"version")=="2.0.0" && V2String(rig,"sha256")==profile.FingerprintSha256,"Manifest profile fingerprint mismatch.");
        var engine=root.GetProperty("engine"); V2Fields(engine,"engineId","version");
        V2Require(V2String(engine,"engineId")=="a0.m2.offline-stitcher" && V2String(engine,"version")=="2.0.0+"+kernel,"Manifest engine/kernel mismatch.");
        var output=root.GetProperty("output"); V2Fields(output,"relativePath","sha256","widthPixels","heightPixels","encodedSizeBytes"); var raster=profile.OutputRaster!;
        V2Require(V2String(output,"relativePath")=="stitched.jpg" && V2String(output,"sha256")==outputHash &&
            V2Integer(output,"widthPixels",32768)==raster.WidthPixels && V2Integer(output,"heightPixels",32768)==raster.HeightPixels &&
            V2Integer(output,"encodedSizeBytes",V2MaximumJpegBytes)==outputSize,"Manifest output binding mismatch.");
        var seam=root.GetProperty("seamNavigation"); V2Require(seam.ValueKind==JsonValueKind.Object && seam.TryGetProperty("available",out var available) &&
            available.ValueKind is JsonValueKind.True or JsonValueKind.False,"Invalid seam availability.");
        if(seam.GetProperty("available").GetBoolean()) { V2Fields(seam,"coordinateSystem","available","xPixels","yPixels");
            V2Require(V2Integer(seam,"xPixels",raster.WidthPixels-1,true)<raster.WidthPixels && V2Integer(seam,"yPixels",raster.HeightPixels-1,true)<raster.HeightPixels,"Seam is outside output dimensions."); }
        else V2Fields(seam,"coordinateSystem","available");
        V2Require(V2String(seam,"coordinateSystem")=="StitchedOutputPixelCenter.v1","Wrong seam coordinate system.");
    }
    private static void V2CheckJfifAndExif(byte[] bytes,int dpi)
    {
        bool jfif=false; int position=2;
        while(position+1<bytes.Length)
        {
            V2Require(bytes[position++]==0xff,"Invalid JPEG metadata marker."); while(position<bytes.Length && bytes[position]==0xff) position++;
            V2Require(position<bytes.Length,"Truncated JPEG metadata."); byte marker=bytes[position++]; if(marker==0xda || marker==0xd9) break;
            V2Require(marker is not (0x00 or 0x01 or 0xd8) && marker is not (>=0xd0 and <=0xd7) && position+2<=bytes.Length,"Unexpected JPEG metadata marker.");
            int length=(bytes[position]<<8)|bytes[position+1]; V2Require(length>=2 && length<=bytes.Length-position,"Invalid JPEG metadata length.");
            var payload=bytes.AsSpan(position+2,length-2);
            if(marker==0xe0 && payload.Length>=5 && payload[..5].SequenceEqual("JFIF\0"u8)) {
                V2Require(!jfif && payload.Length>=14 && payload[7]==1 && ((payload[8]<<8)|payload[9])==dpi && ((payload[10]<<8)|payload[11])==dpi,"JFIF density does not match approved DPI."); jfif=true;
            }
            if(marker==0xe1 && payload.Length>=6 && payload[..6].SequenceEqual("Exif\0\0"u8)) V2RejectExifOrientation(payload[6..]);
            position+=length;
        }
        V2Require(jfif,"Approved-v2 JPEG must carry JFIF density.");
    }
    private static ushort V2ReadU16(ReadOnlySpan<byte> data,int index,bool little)
    { V2Require(index>=0 && index<=data.Length-2,"Truncated TIFF field."); return (ushort)(little ? data[index]|(data[index+1]<<8) : (data[index]<<8)|data[index+1]); }
    private static uint V2ReadU32(ReadOnlySpan<byte> data,int index,bool little)
    { V2Require(index>=0 && index<=data.Length-4,"Truncated TIFF offset."); return little ? (uint)data[index]|((uint)data[index+1]<<8)|((uint)data[index+2]<<16)|((uint)data[index+3]<<24) : ((uint)data[index]<<24)|((uint)data[index+1]<<16)|((uint)data[index+2]<<8)|data[index+3]; }
    private static void V2RejectExifOrientation(ReadOnlySpan<byte> tiff)
    {
        V2Require(tiff.Length>=8 && (tiff[..2].SequenceEqual("II"u8) || tiff[..2].SequenceEqual("MM"u8)),"Invalid EXIF byte order."); bool little=tiff[0]=='I';
        V2Require(V2ReadU16(tiff,2,little)==42,"Invalid TIFF signature."); uint offset=V2ReadU32(tiff,4,little); var visited=new HashSet<uint>();
        while(offset!=0)
        {
            V2Require(offset>=8 && offset<=int.MaxValue && visited.Add(offset),"Invalid or cyclic TIFF IFD."); int start=(int)offset;
            int count=V2ReadU16(tiff,start,little); V2Require((long)start+2+(long)count*12+4<=tiff.Length,"Truncated TIFF IFD.");
            for(int index=0;index<count;index++) V2Require(V2ReadU16(tiff,start+2+index*12,little)!=0x0112,"Output EXIF orientation is forbidden.");
            offset=V2ReadU32(tiff,start+2+count*12,little);
        }
    }
}
