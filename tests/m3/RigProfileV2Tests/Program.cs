using System.Reflection;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using A0CameraStitcher.M3.Foundation.DualCamera;

internal static class Program
{
    private static int checks;
    private static readonly DateTimeOffset Assessment = new(2026,1,1,0,0,0,TimeSpan.Zero);
    private static void Check(bool condition, string message) { checks++; if (!condition) throw new Exception("FAIL: " + message); }
    private static void Reject(Action action, string message)
    {
        bool rejected = false; try { action(); } catch (InvalidDataException) { rejected = true; }
        Check(rejected, message);
    }
    private static string Mutate(string json, Action<JsonObject> change)
    { var root = JsonNode.Parse(json)!.AsObject(); change(root); return root.ToJsonString(); }
    private static JsonObject Projection(JsonObject root) => root["cameras"]!["CAM-A"]!["projection"]!.AsObject();
    private static JsonNode? ReverseObjects(JsonNode? node)
    {
        if (node is JsonObject obj)
        {
            var reversed = new JsonObject();
            foreach (var property in obj.Reverse()) reversed.Add(property.Key, ReverseObjects(property.Value));
            return reversed;
        }
        if (node is JsonArray array) return new JsonArray(array.Select(ReverseObjects).ToArray());
        return node?.DeepClone();
    }
    private static string WithK1(string json, string raw) => new Regex("\"k1\"\\s*:\\s*[-+\\deE.]+").Replace(json, "\"k1\":" + raw, 1);
    private static string WithRms(string json, string raw) => new Regex("\"rms\"\\s*:\\s*[-+\\deE.]+").Replace(json, "\"rms\":" + raw, 1);
    private static string Raster(string left = "0", string right = "0.1", string width = "1", string dpi = "100") =>
        "{\"dpi\":"+dpi+",\"regionMm\":{\"left\":"+left+",\"top\":0,\"right\":"+right+",\"bottom\":0.1},\"widthPixels\":"+width+",\"heightPixels\":1}";
    private static void CheckReadOnly(Type type)
    {
        foreach (var property in type.GetProperties(BindingFlags.Instance | BindingFlags.Public))
            Check(property.SetMethod is null, type.Name + "." + property.Name + " has no exposed setter");
    }
    private static void TestVectors(string fixtureRoot)
    {
        foreach (string name in new[] { "template", "calibrated-draft", "approved" })
        {
            var json = File.ReadAllText(Path.Combine(fixtureRoot,name+".json")); var profile = RigProfileV2.Parse(json);
            Check(profile.CanonicalFingerprintText == File.ReadAllText(Path.Combine(fixtureRoot,name+".canonical.txt")), name+" canonical lines match independent shared fixture");
            Check(profile.FingerprintSha256 == File.ReadAllText(Path.Combine(fixtureRoot,name+".sha256")).Trim(), name+" SHA-256 matches shared fixture");
            Check(profile.CanonicalFingerprintText.EndsWith('\n') && !profile.CanonicalFingerprintText.Contains('\r'), "canonical final LF and no CR");
            var reordered = ReverseObjects(JsonNode.Parse(json))!;
            Check(RigProfileV2.Parse(reordered.ToJsonString()).FingerprintSha256 == profile.FingerprintSha256, "recursive object order and formatting do not affect fingerprint; matrix arrays keep their order");
        }
        var draft = File.ReadAllText(Path.Combine(fixtureRoot,"calibrated-draft.json"));
        using var numeric = JsonDocument.Parse(File.ReadAllText(Path.Combine(fixtureRoot,"number-cases.json")));
        foreach (var item in numeric.RootElement.EnumerateArray())
        {
            string literal = item.GetProperty("jsonNumber").GetString()!, hex = item.GetProperty("binary64Hex").GetString()!;
            var profile = RigProfileV2.Parse(WithK1(draft,literal));
            Check(profile.CanonicalFingerprintText.Contains("/cameras/CAM-A/projection/distortion/k1=f:"+hex+"\n",StringComparison.Ordinal), "binary64 nearest-even known answer: "+literal);
        }
        Check(RigProfileV2.Parse(WithK1(draft,"-0")).FingerprintSha256 == RigProfileV2.Parse(WithK1(draft,"0.0")).FingerprintSha256, "negative zero is canonical positive zero");
        Check(RigProfileV2.Parse(WithRms(draft,"-0e-400")).FingerprintSha256 == RigProfileV2.Parse(draft).FingerprintSha256, "nonnegative scalar accepts exact signed zero and canonicalizes it");
        Check(RigProfileV2.Parse(WithK1(draft,"0.1")).FingerprintSha256 != RigProfileV2.Parse(draft).FingerprintSha256, "changed numeric content changes fingerprint");
    }
    private static void TestParsing(string template, string draft, string approved)
    {
        Reject(() => RigProfileV2.Parse(template.Replace("\"schemaVersion\":", "\"schemaVersion\":\"2.0.0\",\"schemaVersion\":")), "duplicate root key");
        Reject(() => RigProfileV2.Parse(template.Replace("\"schemaVersion\":", "\"\\u0073chemaVersion\":\"2.0.0\",\"schemaVersion\":")), "escaped duplicate key compares decoded ordinal names");
        Reject(() => RigProfileV2.Parse(WithK1(draft,"1e400")), "overflowing binary64 is rejected");
        Reject(() => RigProfileV2.Parse(WithK1(draft,"NaN")), "NaN is not JSON");
        Reject(() => RigProfileV2.Parse(WithK1(draft,"Infinity")), "Infinity is not JSON");
        Reject(() => RigProfileV2.Parse(WithRms(draft,"-1e-400")), "nonnegative scalar cannot conceal a mathematically negative underflow");
        var negativeTarget = new Regex("\"targetMax\"\\s*:\\s*[-+\\deE.]+").Replace(approved,"\"targetMax\":-1e-400",1);
        Reject(() => RigProfileV2.Parse(negativeTarget), "nonnegative correction target cannot conceal negative underflow");
        var zeroTarget = new Regex("\"targetMax\"\\s*:\\s*[-+\\deE.]+").Replace(approved,"\"targetMax\":-0e-400",1);
        Check(RigProfileV2.Parse(zeroTarget).FingerprintSha256 == RigProfileV2.Parse(approved).FingerprintSha256, "exact signed-zero correction target remains canonical zero");
        Reject(() => RigProfileV2.Parse(draft.Replace("\"k1\":", "\"k1\":0,\"k1\":")), "nested duplicate key");
        Reject(() => RigProfileV2.Parse(Mutate(template,r=>r["unknown"]=true)), "unknown root field");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>Projection(r)["intrinsics"]!["unknown"]=0)), "unknown nested field");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>Projection(r)["distortion"]!.AsObject().Remove("p2"))), "missing tangential coefficient is not default zero");
        Reject(() => RigProfileV2.Parse(Mutate(template,r=>r["schemaVersion"]="1.1.0")), "v1 schema cannot be read as v2");
        Reject(() => RigProfileV2.Parse(Mutate(template,r=>r["cameraModel"]!["sensorWidthPixels"]=7361)), "D810 dimensions are fixed");
        Reject(() => RigProfileV2.Parse(template.Replace("7360", "7360.0")), "integer camera dimension rejects floating lexical form");
        Reject(() => RigProfileV2.Parse(template.Replace("7360", "7360e0")), "integer camera dimension rejects exponent lexical form");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>r["profileId"]="identifier=not-allowed")), "ASCII identifier cannot alter canonical lines");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>r["profileId"]="非ASCII")), "identifier is ASCII only");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>r["cameras"]!["CAM-A"]!["physicalIdentity"]="synthetic-forbidden")), "physical identity is forbidden");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>r["cameras"]!["CAM-B"]!["projection"]=null)), "partial calibration draft is rejected");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>r["outputRaster"]=JsonNode.Parse(approved)!["outputRaster"]!.DeepClone())), "draft cannot carry owner raster");
        Reject(() => RigProfileV2.Parse(Mutate(approved,r=>r["approval"]=null)), "approved cannot omit approval");
        Reject(() => RigProfileV2.Parse(Mutate(approved,r=>r["calibration"]!["rigMeasurements"]!["baselineMm"]=null)), "approved rig measurement cannot be null");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>Projection(r)["intrinsics"]!["fxPixels"]=0)), "focal length zero");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>Projection(r)["documentToImage"]![2]![2]=2)), "h22 normalization");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>Projection(r)["documentToImage"]=new JsonArray(new JsonArray(0,0,0),new JsonArray(0,0,0),new JsonArray(0,0,1)))), "singular matrix");
        Reject(() => RigProfileV2.Parse(Mutate(draft,r=>Projection(r)["documentToImage"]=new JsonArray(new JsonArray(1,0,0),new JsonArray(0,1,0)))), "matrix is exactly 3 by 3");
        foreach (string badDate in new[] { "0000-01-01T00:00:00Z", "2001-02-29T00:00:00Z", "2000-02-30T00:00:00Z", "2000-01-01T00:00:60Z", "2000-01-01T00:00:00+00:00", "2000-01-01T00:00:00.0Z" })
            Reject(() => RigProfileV2.Parse(Mutate(draft,r=>r["calibration"]!["measuredAt"]=badDate)), "invalid Gregorian/UTC time "+badDate);
        var leap=RigProfileV2.Parse(Mutate(draft,r=>r["calibration"]!["measuredAt"]="2000-02-29T00:00:00Z"));
        Check(leap.Calibration!.MeasuredAt.Day==29, "Gregorian leap day accepted");
        Reject(() => RigProfileV2.Parse(new string(' ',256*1024+1)+template), "UTF8 resource limit");
        Reject(() => RigProfileV2.Parse("{\"unknown\":\""+new string('\u00e9',132000)+"\"}"), "UTF8 byte limit is not character count");
        Reject(() => RigProfileV2.Parse(template+"\ud800"), "unpaired UTF16 surrogate cannot be replaced during UTF8 conversion");
        Reject(() => RigProfileV2.Parse(new string('[',33)+"0"+new string(']',33)), "nesting depth bound");
        Reject(() => RigProfileV2.Parse(template+"{}"), "trailing document rejected");
        Reject(() => RigProfileV2.Parse(template.Replace("\"schemaVersion\"", "/*comment*/\"schemaVersion\"")), "comments rejected");
    }
    private static void TestUse(string template, string draft, string approved)
    {
        var profile=RigProfileV2.Parse(approved); string fingerprint=profile.FingerprintSha256;
        profile.ValidateApprovedForUse(Assessment); profile.ValidateApprovedForUse(Assessment.AddDays(1));
        Check(profile.FingerprintSha256==fingerprint, "assessment time does not alter fingerprint");
        Reject(()=>profile.ValidateApprovedForUse(profile.Approval!.ValidUntil), "expiry instant is excluded");
        profile.ValidateApprovedForUse(profile.Approval!.ApprovedAt); Check(true,"approval instant is included");
        Reject(()=>profile.ValidateApprovedForUse(profile.Approval!.ApprovedAt.AddSeconds(-1)), "before approval rejected");
        Reject(()=>profile.ValidateApprovedForUse(Assessment.ToOffset(TimeSpan.FromHours(1))), "assessment must be expressed as UTC");
        Reject(()=>RigProfileV2.Parse(template).ValidateApprovedForUse(Assessment), "draft template is never a product profile");
        var calibrated=RigProfileV2.Parse(draft); Reject(()=>calibrated.ValidateApprovedForUse(Assessment), "calibrated draft is never a product profile");
        calibrated.ValidateCalibrationForEvaluation(profile.OutputRaster!); Check(true,"calibrated draft uses external raster");
        Reject(()=>RigProfileV2.Parse(template).ValidateCalibrationForEvaluation(profile.OutputRaster!), "template has no evaluation geometry");
        Reject(()=>profile.ValidateCalibrationForEvaluation(profile.OutputRaster!), "evaluation method accepts draft only");
        Reject(()=>RigProfileV2.Parse(Mutate(approved,r=>r["outputRaster"]!["widthPixels"]=8425)).ValidateApprovedForUse(Assessment), "exact ceil mismatch");
        Reject(()=>RigProfileV2.Parse(Mutate(approved,r=>r["outputRaster"]!["regionMm"]!["right"]=1200)).ValidateApprovedForUse(Assessment), "raster cannot extend outside sheet");
        Reject(()=>RigProfileV2.Parse(Mutate(approved,r=>r["correctionEnvelope"]!["rotationErrorDegrees"]!["targetMax"]=1)), "target cannot exceed correction maximum");
        Reject(()=>RigProfileV2.Parse(Mutate(approved,r=>r["approval"]!["approvedAt"]="1999-01-01T00:00:00Z")), "approval cannot precede calibration");
        var horizon=RigProfileV2.Parse(Mutate(draft,r=>Projection(r)["documentToImage"]=new JsonArray(new JsonArray(1,0,0),new JsonArray(0,1,0),new JsonArray(-8,0,1))));
        Reject(()=>horizon.ValidateCalibrationForEvaluation(new(0,0,100,100,100,1,1)), "ceil outer edges crossing horizon are rejected even if declared region corners are positive");
        foreach (double k3 in new[] { 0.0,1.0/7 })
        {
            var valley=RigProfileV2.Parse(Mutate(draft,r=> {
                Projection(r)["intrinsics"]=new JsonObject { ["fxPixels"]=1,["fyPixels"]=1,["cxPixels"]=0,["cyPixels"]=0 };
                Projection(r)["documentToImage"]=new JsonArray(new JsonArray(1,0,0),new JsonArray(0,1,0),new JsonArray(0,0,1));
                var lens=Projection(r)["distortion"]!; lens["k1"]=-2; lens["k2"]=1.2; lens["k3"]=k3;
            }));
            Reject(()=>valley.ValidateCalibrationForEvaluation(new(0,0,707,707,254,8,8)), "radial derivative internal negative valley for quadratic and cubic polynomials");
        }
        var immutableCamera = profile.Cameras.CameraA!;
        Check(immutableCamera.DocumentToImage.Length==9, "immutable matrix has nine entries");
        var changed=immutableCamera.DocumentToImage.SetItem(0,123);
        Check(changed[0]==123 && immutableCamera.DocumentToImage[0]!=123 && profile.FingerprintSha256==fingerprint, "matrix replacement cannot mutate the parsed profile");
        foreach(var type in new[] { typeof(RigProfileV2),typeof(RigPairV2<double?>),typeof(RigIntrinsicsV2),typeof(RigLensDistortionV2),typeof(RigCameraProjectionV2),typeof(RigDocumentPlaneV2),typeof(RigCalibrationV2),typeof(RigMeasurementsV2),typeof(RigResidualV2),typeof(RigOutputRasterV2),typeof(RigCorrectionEnvelopeV2),typeof(RigEnvelopeMetricV2),typeof(RigQualityContractV2),typeof(RigApprovalV2) }) CheckReadOnly(type);
    }
    private static void TestExactRaster()
    {
        Check(RigOutputRasterV2.Parse(Raster("1e-3")).LeftUm==1, "exact decimal exponent micrometre accepted");
        Check(RigOutputRasterV2.Parse(Raster("0.001000")).LeftUm==1, "exact trailing zeros accepted");
        Reject(()=>RigOutputRasterV2.Parse(Raster("0.0010000000000000000001")), "binary64 rounding cannot conceal non-micrometre input");
        Reject(()=>RigOutputRasterV2.Parse(Raster("1e-400")), "underflow cannot conceal a nonzero fractional micrometre");
        Reject(()=>RigOutputRasterV2.Parse(Raster("-1e-400")), "negative underflow is not a nonnegative region value");
        Reject(()=>RigOutputRasterV2.Parse(Raster(width:"1.0")), "raster dimensions require integer lexical form");
        Reject(()=>RigOutputRasterV2.Parse(Raster(dpi:"100e0")), "DPI requires integer lexical form");
        Reject(()=>RigOutputRasterV2.Parse(Raster(dpi:"0")), "DPI zero rejected");
        Reject(()=>RigOutputRasterV2.Parse(Raster(right:"0")), "empty region rejected");
        Reject(()=>new RigOutputRasterV2(0,0,3276900,100,254,32769,1), "dimension beyond engine bound");
        Reject(()=>new RigOutputRasterV2(0,0,2000000,2000000,254,20000,20000), "total pixel bound");
        Reject(()=>new RigOutputRasterV2(0,0,long.MaxValue,100,65535,1,1), "extreme arithmetic fails exact dimension check without wrapping");
    }
    public static int Main(string[] args)
    {
        try
        {
            if(args.Length!=1) throw new ArgumentException("One shared fixture directory is required.");
            string root=Path.GetFullPath(args[0]); var template=File.ReadAllText(Path.Combine(root,"template.json"));
            var draft=File.ReadAllText(Path.Combine(root,"calibrated-draft.json")); var approved=File.ReadAllText(Path.Combine(root,"approved.json"));
            TestVectors(root); TestParsing(template,draft,approved); TestUse(template,draft,approved); TestExactRaster();
            Console.WriteLine($"rig_profile_v2_dotnet checks={checks} failures=0"); return 0;
        }
        catch(Exception error) { Console.Error.WriteLine(error); return 1; }
    }
}
