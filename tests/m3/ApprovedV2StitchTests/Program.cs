using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using A0CameraStitcher.M3.Foundation.DualCamera;
using Microsoft.Win32.SafeHandles;

internal static class Program
{
    private const string FakeSourceVariable="A0_V2_STITCH_TEST_SOURCE";
    private const string FakeFaultVariable="A0_V2_STITCH_TEST_FAULT";
    private static readonly DateTimeOffset Assessed=new(2026,1,1,0,0,0,TimeSpan.Zero);
    private static int checks;
    [DllImport("kernel32.dll",EntryPoint="CreateFileW",CharSet=CharSet.Unicode,SetLastError=true)]
    private static extern SafeFileHandle OpenDeleteProbe(string path,uint access,uint sharing,IntPtr security,uint creation,uint flags,IntPtr template);
    private static void ProbeLocked(string path)
    {
        bool blocked=false;
        try { using var writable=File.Open(path,FileMode.Open,FileAccess.Write,FileShare.ReadWrite|FileShare.Delete); }
        catch(IOException error) when((error.HResult&0xffff)==32) { blocked=true; }
        if(!blocked) throw new Exception("Managed lock did not deny child write access.");
        // Request DELETE permission without setting DeleteOnClose or deleting
        // data. A denied handle proves deletion/rename is excluded by sharing.
        using var deletion=OpenDeleteProbe(path,0x10000,7,IntPtr.Zero,3,0x00200000,IntPtr.Zero);
        if(!deletion.IsInvalid || Marshal.GetLastWin32Error()!=32) throw new Exception("Managed lock did not deny child delete access.");
    }
    private static void Check(bool value,string message) { checks++; if(!value) throw new Exception("FAIL: "+message); }
    private static async Task Reject(Func<Task> action,string message)
    {
        bool rejected=false; try { await action(); } catch(Exception error) when(error is InvalidDataException or IOException or ArgumentException or InvalidOperationException) { rejected=true; }
        Check(rejected,message);
    }
    private static async Task<string> Native(string executable,params string[] arguments)
    {
        var start=new ProcessStartInfo(executable) { UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true };
        foreach(var arg in arguments) start.ArgumentList.Add(arg);
        using var process=new Process { StartInfo=start }; if(!process.Start()) throw new Exception("Process did not start.");
        var output=process.StandardOutput.ReadToEndAsync(); var error=process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync(); string text=await output, diagnostic=await error;
        if(process.ExitCode!=0) throw new Exception($"Test CLI failed: exit {process.ExitCode}: {diagnostic}"); return text;
    }
    private static CanonicalJpegOriginal Original(string alias,string path)
    {
        var bytes=File.ReadAllBytes(path);
        return new(alias,path,bytes.LongLength,Convert.ToHexStringLower(SHA256.HashData(bytes)),7360,4912,true);
    }
    private static byte[] AddOrientation(byte[] source)
    {
        byte[] exif=[(byte)'E',(byte)'x',(byte)'i',(byte)'f',0,0,(byte)'I',(byte)'I',42,0,8,0,0,0,
            1,0,0x12,1,3,0,1,0,0,0,1,0,0,0,0,0,0,0];
        var bytes=new byte[source.Length+exif.Length+4]; source.AsSpan(0,2).CopyTo(bytes);
        bytes[2]=0xff;bytes[3]=0xe1;bytes[4]=0;bytes[5]=(byte)(exif.Length+2);
        exif.CopyTo(bytes,6);source.AsSpan(2).CopyTo(bytes.AsSpan(6+exif.Length));return bytes;
    }
    private static void ChangeDensity(byte[] bytes)
    {
        for(int i=0;i<bytes.Length-12;i++) if(bytes.AsSpan(i,5).SequenceEqual("JFIF\0"u8)) { bytes[i+8]=0;bytes[i+9]=99;return; }
        throw new Exception("Positive native JPEG has no JFIF marker.");
    }
    // The apphost doubles as a deliberately dishonest child. It copies a real
    // native output, forges one binding, and prints success. The managed method
    // must independently refuse it, even when fake verification also says OK.
    private static int FakeChild(string[] args)
    {
        var options=new Dictionary<string,string>(StringComparer.Ordinal);
        for(int i=1;i<args.Length;i+=2) options.Add(args[i],args[i+1]);
        string directory=options.GetValueOrDefault("--job-directory") ?? "";
        if(args[0]=="validate-canonical-jpeg") { Console.WriteLine("result=validated-canonical-jpeg");return 0; }
        if(args[0]=="verify-published-stitch") {
            Console.WriteLine("result=verified-published-stitch");Console.WriteLine("manifestJson="+File.ReadAllText(Path.Combine(directory,"stitch-job.manifest.json")).Replace("\r","").Replace("\n",""));return 0;
        }
        string source=Environment.GetEnvironmentVariable(FakeSourceVariable) ?? throw new Exception("Missing fake source.");
        string fault=Environment.GetEnvironmentVariable(FakeFaultVariable) ?? throw new Exception("Missing fake fault.");
        if(fault=="unchanged") foreach(string key in new[]{"--profile-file","--camera-a","--camera-b"}) ProbeLocked(options[key]);
        Directory.CreateDirectory(directory);byte[] jpeg=File.ReadAllBytes(Path.Combine(source,"stitched.jpg"));
        var manifest=JsonNode.Parse(File.ReadAllText(Path.Combine(source,"stitch-job.manifest.json")))!.AsObject();
        manifest["stitchJobId"]=options["--stitch-job-id"];manifest["captureTransactionId"]=options["--capture-transaction-id"];
        manifest["completedAtUtc"]=options["--completed-at"];
        manifest["rigProfile"]!["sha256"]=options["--expected-profile-sha256"];
        manifest["engine"]!["version"]="2.0.0+"+options["--resampling"];
        switch(fault) {
            case "profile-hash":manifest["rigProfile"]!["sha256"]=new string('d',64);break;
            case "kernel":manifest["engine"]!["version"]="2.0.0+bicubic-catmull-rom";break;
            case "input-hash":manifest["inputs"]![0]!["sha256"]=new string('d',64);break;
            case "input-order":var saved=manifest["inputs"]![0]!.DeepClone();manifest["inputs"]![0]=manifest["inputs"]![1]!.DeepClone();manifest["inputs"]![1]=saved;break;
            case "capture-id":manifest["captureTransactionId"]=new string('0',32);break;
            case "dimensions":manifest["output"]!["widthPixels"]=5;break;
            case "completed":manifest["completedAtUtc"]="2025-01-01T00:00:00Z";break;
            case "retry":manifest["automaticRetryCount"]=1;break;
            case "unknown":manifest["unexpected"]=true;break;
            case "seam":manifest["seamNavigation"]!["available"]=true;manifest["seamNavigation"]!["xPixels"]=4;manifest["seamNavigation"]!["yPixels"]=0;break;
            case "density":ChangeDensity(jpeg);break;
            case "orientation":jpeg=AddOrientation(jpeg);break;
        }
        manifest["output"]!["encodedSizeBytes"]=jpeg.LongLength;
        manifest["output"]!["sha256"]=Convert.ToHexStringLower(SHA256.HashData(jpeg));
        File.WriteAllBytes(Path.Combine(directory,"stitched.jpg"),jpeg);
        string json=manifest.ToJsonString();
        if(fault=="duplicate") json=json.Replace("\"schemaVersion\":","\"schemaVersion\":\"a0.stitch-job-manifest.v2\",\"schemaVersion\":",StringComparison.Ordinal);
        if(fault!="missing-manifest") File.WriteAllText(Path.Combine(directory,"stitch-job.manifest.json"),json,new UTF8Encoding(false));
        Console.WriteLine("result=stitched\nwidth=4\nheight=4\nprofileId=synthetic-approved-product-crop\nstitchJobId="+
            (fault=="stdout-id" ? new string('0',32) : options["--stitch-job-id"])+"\nmanifest=stitch-job.manifest.json");return 0;
    }
    private static async Task Run(string fixtureDirectory,string nativeExe,string root)
    {
        string json=File.ReadAllText(Path.Combine(fixtureDirectory,"approved-product-crop.json"));var profile=RigProfileV2.Parse(json);
        Check(profile.FingerprintSha256==File.ReadAllText(Path.Combine(fixtureDirectory,"approved-product-crop.sha256")).Trim(),"small product fixture SHA golden");
        Check(profile.CanonicalFingerprintText==File.ReadAllText(Path.Combine(fixtureDirectory,"approved-product-crop.canonical.txt")),"small product fixture canonical golden");
        string profilePath=Path.Combine(root,"approved.json");File.WriteAllText(profilePath,json,new UTF8Encoding(false));
        var paths=new[]{Path.Combine(root,"CAM-A","original.jpg"),Path.Combine(root,"CAM-B","original.jpg")};
        foreach(string path in paths) Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        await Native(nativeExe,"generate-test-synthetic","--alias","CAM-A","--output",paths[0],"--width","7360","--height","4912");
        await Native(nativeExe,"generate-test-synthetic","--alias","CAM-B","--output",paths[1],"--width","7360","--height","4912");
        CanonicalJpegOriginal[] originals=[Original("CAM-A",paths[0]),Original("CAM-B",paths[1])];
        var adapter=new M2OfflineStitcherProcessAdapter(nativeExe);string? goodDirectory=null;
        foreach(var kernel in new[]{V2StitchResampling.Bilinear,V2StitchResampling.BicubicCatmullRom})
        {
            string jobDirectory=Path.Combine(root,"real-"+kernel);var job=Guid.NewGuid();var capture=Guid.NewGuid();
            var artifact=await adapter.StitchV2Async(originals,jobDirectory,profilePath,profile,Assessed,kernel,job,capture,Assessed,CancellationToken.None);
            Check(artifact.Width==4 && artifact.Height==4 && artifact.ProfileId==profile.ProfileId && artifact.ManifestFileName=="stitch-job.manifest.json","actual native v2 result through .NET for each kernel");
            var manifest=JsonNode.Parse(File.ReadAllText(Path.Combine(jobDirectory,artifact.ManifestFileName)))!;
            Check(manifest["rigProfile"]!["sha256"]!.GetValue<string>()==profile.FingerprintSha256 && manifest["rigProfile"]!["version"]!.GetValue<string>()=="2.0.0","native computed immutable profile binding");
            Check(manifest["stitchJobId"]!.GetValue<string>()==job.ToString("N") && manifest["captureTransactionId"]!.GetValue<string>()==capture.ToString("N"),"manifest binds requested IDs");
            string jpegHash=Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(artifact.OutputPath)));
            Check(manifest["output"]!["sha256"]!.GetValue<string>()==jpegHash,"published JPEG hash agrees with manifest");
            await Reject(()=>adapter.StitchV2Async(originals,jobDirectory,profilePath,profile,Assessed,kernel,Guid.NewGuid(),capture,Assessed,CancellationToken.None),"existing job is never replaced");
            Check(jpegHash==Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(artifact.OutputPath))),"nonreplace rejection preserves output");
            if(kernel==V2StitchResampling.Bilinear) goodDirectory=jobDirectory;
        }
        string NewJob()=>Path.Combine(root,"negative-"+Guid.NewGuid().ToString("N"));
        Task<OfflineStitchArtifact> Call(string directory,RigProfileV2 expected,CanonicalJpegOriginal[] pair,Guid? job=null,Guid? capture=null,
            V2StitchResampling kernel=V2StitchResampling.Bilinear,DateTimeOffset? time=null) =>
            adapter.StitchV2Async(pair,directory,profilePath,expected,time??Assessed,kernel,job??Guid.NewGuid(),capture??Guid.NewGuid(),time??Assessed,CancellationToken.None);
        var mismatch=RigProfileV2.Parse(json.Replace("synthetic-approved-product-crop","synthetic-other-profile",StringComparison.Ordinal));
        string refused=NewJob();await Reject(()=>Call(refused,mismatch,originals),"expected immutable profile mismatch rejected before publish");Check(!Directory.Exists(refused),"profile mismatch leaves no job");
        await Reject(()=>Call(NewJob(),RigProfileV2.Parse(File.ReadAllText(Path.Combine(fixtureDirectory,"calibrated-draft.json"))),originals),"draft cannot use production v2 method");
        await Reject(()=>Call(NewJob(),profile,[originals[0] with{Sha256=new string('c',64)},originals[1]]),"input metadata mismatch");
        await Reject(()=>Call(NewJob(),profile,[originals[1],originals[0]]),"input order is explicit");
        await Reject(()=>Call(NewJob(),profile,originals,job:Guid.Empty),"empty job ID");
        await Reject(()=>Call(NewJob(),profile,originals,capture:Guid.Empty),"empty capture ID");
        await Reject(()=>Call(NewJob(),profile,originals,kernel:(V2StitchResampling)99),"unsupported kernel");
        await Reject(()=>Call(NewJob(),profile,originals,time:profile.Approval!.ValidUntil),"expired profile");
        await Reject(()=>Call(NewJob(),profile,originals,time:Assessed.AddTicks(1)),"fractional UTC is not silently truncated");
        await Reject(()=>Call(Path.Combine(root,"NUL.json"),profile,originals),"reserved device path");
        await Reject(()=>Call(Path.Combine(root,"report:ads"),profile,originals),"alternate data stream path");
        await Reject(()=>Call(Path.Combine(root,"trailing."),profile,originals),"trailing-dot path");
        string junction=Path.Combine(root,"redirect"), target=Path.Combine(root,"redirect-target");Directory.CreateDirectory(target);
        Directory.CreateSymbolicLink(junction,target);
        try { await Reject(()=>Call(Path.Combine(junction,"new-job"),profile,originals),"reparse parent cannot redirect a job"); }
        finally { Directory.Delete(junction); }
        string fakeExe=Path.Combine(AppContext.BaseDirectory,"A0CameraStitcher.M3.ApprovedV2StitchTests.exe");
        Check(File.Exists(fakeExe),"managed test apphost exists for forged-child cases");var forged=new M2OfflineStitcherProcessAdapter(fakeExe);
        string? oldSource=Environment.GetEnvironmentVariable(FakeSourceVariable),oldFault=Environment.GetEnvironmentVariable(FakeFaultVariable);
        try {
            Environment.SetEnvironmentVariable(FakeSourceVariable,goodDirectory!);
            // A clean control prevents child startup or fixture errors from
            // making every forged-binding rejection pass without its assertion.
            foreach(string path in paths.Append(profilePath)) {
                using var writable=File.Open(path,FileMode.Open,FileAccess.Write,FileShare.ReadWrite|FileShare.Delete);
                using var deletion=OpenDeleteProbe(path,0x10000,7,IntPtr.Zero,3,0x00200000,IntPtr.Zero);
                Check(!deletion.IsInvalid,"owned fixture permits delete access outside adapter lock");
            }
            Environment.SetEnvironmentVariable(FakeFaultVariable,"unchanged");
            var controlled=await forged.StitchV2Async(originals,NewJob(),profilePath,profile,Assessed,V2StitchResampling.Bilinear,Guid.NewGuid(),Guid.NewGuid(),Assessed,CancellationToken.None);
            Check(controlled.Width==4 && controlled.Height==4,"unchanged child control passes and observes all profile/input write/delete locks");
            foreach(string fault in new[]{"profile-hash","kernel","input-hash","input-order","capture-id","dimensions","completed","retry","unknown","seam","density","orientation","duplicate","missing-manifest","stdout-id"}) {
                Environment.SetEnvironmentVariable(FakeFaultVariable,fault);string directory=NewJob();
                string expectedReason=fault switch {
                    "profile-hash"=>"Manifest profile fingerprint mismatch.","kernel"=>"Manifest engine/kernel mismatch.",
                    "input-hash" or "input-order"=>"Manifest original binding mismatch.",
                    "capture-id" or "completed"=>"Manifest identity, time or result state mismatch.",
                    "dimensions"=>"Manifest output binding mismatch.","unknown" or "duplicate"=>"Missing, duplicated or unknown manifest fields.",
                    "seam" or "retry"=>"Manifest integer is outside its bounds.","density"=>"JFIF density does not match approved DPI.",
                    "orientation"=>"Output EXIF orientation is forbidden.","missing-manifest"=>"Required regular file is unavailable.",
                    "stdout-id"=>"Native stitch response does not match the requested profile, job or output.",_=>throw new Exception("Unspecified rejection reason.")
                };
                bool rejected=false;
                try { await forged.StitchV2Async(originals,directory,profilePath,profile,Assessed,V2StitchResampling.Bilinear,Guid.NewGuid(),Guid.NewGuid(),Assessed,CancellationToken.None); }
                catch(InvalidDataException error) { Check(error.Message==expectedReason,"specific managed refusal: "+fault+"; actual: "+error.Message);rejected=true; }
                Check(rejected,"forged success rejected: "+fault);
                Check(File.Exists(Path.Combine(directory,"stitched.jpg")),"dishonest child actually published its fixture: "+fault);
                Check(File.Exists(Path.Combine(directory,"stitch-job.manifest.json"))==(fault!="missing-manifest"),"dishonest manifest fixture exists as intended: "+fault);
            }
        }
        finally { Environment.SetEnvironmentVariable(FakeSourceVariable,oldSource);Environment.SetEnvironmentVariable(FakeFaultVariable,oldFault); }
        for(int index=0;index<2;index++) Check(Original(originals[index].Alias,paths[index]).Sha256==originals[index].Sha256,"every successful and failed attempt preserves each original");
        Check(File.ReadAllText(profilePath)==json,"profile file is never rewritten");
    }
    public static async Task<int> Main(string[] args)
    {
        if(args.Length>0 && args[0] is "stitch-v2" or "verify-published-stitch" or "validate-canonical-jpeg") {
            try { return FakeChild(args); } catch(Exception error) { Console.Error.WriteLine(error.Message);return 2; }
        }
        string root=Path.Combine(Path.GetTempPath(),"a0-approved-v2-test-"+Guid.NewGuid().ToString("N"));
        try {
            if(args.Length!=2) throw new ArgumentException("Shared fixture directory and native M2 adapter executable are required.");
            Directory.CreateDirectory(root);await Run(Path.GetFullPath(args[0]),Path.GetFullPath(args[1]),root);
            Console.WriteLine($"approved_v2_stitch checks={checks} failures=0");return 0;
        }
        catch(Exception error) { Console.Error.WriteLine(error);return 1; }
        finally {
            string full=Path.GetFullPath(root),temporaryBase=Path.GetFullPath(Path.GetTempPath()).TrimEnd(Path.DirectorySeparatorChar);
            if(Directory.Exists(full) && full.StartsWith(temporaryBase+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase) && Path.GetFileName(full).StartsWith("a0-approved-v2-test-",StringComparison.Ordinal)) Directory.Delete(full,true);
        }
    }
}
