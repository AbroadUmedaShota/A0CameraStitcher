using System.Collections.Immutable;
using System.Globalization;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace A0CameraStitcher.M3.Foundation.DualCamera;

public sealed record RigPairV2<T>
{
    public T CameraA { get; }
    public T CameraB { get; }
    internal RigPairV2(T a, T b) { CameraA = a; CameraB = b; }
}
public sealed record RigIntrinsicsV2
{
    public double FxPixels { get; } public double FyPixels { get; }
    public double CxPixels { get; } public double CyPixels { get; }
    internal RigIntrinsicsV2(double fx, double fy, double cx, double cy) { FxPixels = fx; FyPixels = fy; CxPixels = cx; CyPixels = cy; }
}
public sealed record RigLensDistortionV2
{
    public double K1 { get; } public double K2 { get; } public double K3 { get; }
    public double P1 { get; } public double P2 { get; }
    internal RigLensDistortionV2(double k1, double k2, double k3, double p1, double p2) { K1 = k1; K2 = k2; K3 = k3; P1 = p1; P2 = p2; }
}
public sealed record RigCameraProjectionV2
{
    public RigIntrinsicsV2 Intrinsics { get; } public RigLensDistortionV2 Distortion { get; }
    public ImmutableArray<double> DocumentToImage { get; }
    internal RigCameraProjectionV2(RigIntrinsicsV2 intrinsics, RigLensDistortionV2 lens, ImmutableArray<double> matrix)
    { Intrinsics = intrinsics; Distortion = lens; DocumentToImage = matrix; }
}
public sealed record RigDocumentPlaneV2
{
    public string Orientation { get; } public string CameraOrder { get; }
    public double SheetWidthMm { get; } public double SheetHeightMm { get; }
    internal RigDocumentPlaneV2(string orientation, string order, double width, double height)
    { Orientation = orientation; CameraOrder = order; SheetWidthMm = width; SheetHeightMm = height; }
}
public sealed record RigResidualV2
{
    public double Rms { get; } public double Max { get; }
    internal RigResidualV2(double rms, double max) { Rms = rms; Max = max; }
}
public sealed record RigMeasurementsV2
{
    public double? BaselineMm { get; } public double? OverlapMm { get; }
    public RigPairV2<double?> CameraToDocumentMm { get; } public RigPairV2<double?> LensFocalLengthMm { get; }
    internal RigMeasurementsV2(double? baseline, double? overlap, RigPairV2<double?> distance, RigPairV2<double?> focal)
    { BaselineMm = baseline; OverlapMm = overlap; CameraToDocumentMm = distance; LensFocalLengthMm = focal; }
}
public sealed record RigCalibrationV2
{
    public string ChartId { get; } public string ProvenanceId { get; } public DateTimeOffset MeasuredAt { get; }
    public string ToolId { get; } public string ToolVersion { get; }
    public RigPairV2<string> InputSha256 { get; } public RigPairV2<RigResidualV2> FitResidualPixels { get; }
    public RigMeasurementsV2 RigMeasurements { get; }
    internal RigCalibrationV2(string chart, string provenance, DateTimeOffset measured, string tool, string version,
        RigPairV2<string> hashes, RigPairV2<RigResidualV2> residual, RigMeasurementsV2 measurements)
    { ChartId = chart; ProvenanceId = provenance; MeasuredAt = measured; ToolId = tool; ToolVersion = version;
      InputSha256 = hashes; FitResidualPixels = residual; RigMeasurements = measurements; }
}
public sealed record RigEnvelopeMetricV2
{
    public double TargetMax { get; } public double AutoCorrectionMax { get; }
    internal RigEnvelopeMetricV2(double target, double auto) { TargetMax = target; AutoCorrectionMax = auto; }
}
public sealed record RigCorrectionEnvelopeV2
{
    public double MinimumOverlapMm { get; }
    public RigEnvelopeMetricV2 RegistrationErrorOutputPixels { get; } public RigEnvelopeMetricV2 RotationErrorDegrees { get; }
    public RigEnvelopeMetricV2 ScaleDifferencePercent { get; } public RigEnvelopeMetricV2 ExposureDifferenceEv { get; }
    public RigEnvelopeMetricV2 ColorDeltaE { get; }
    internal RigCorrectionEnvelopeV2(double overlap, RigEnvelopeMetricV2 registration, RigEnvelopeMetricV2 rotation,
        RigEnvelopeMetricV2 scale, RigEnvelopeMetricV2 exposure, RigEnvelopeMetricV2 color)
    { MinimumOverlapMm = overlap; RegistrationErrorOutputPixels = registration; RotationErrorDegrees = rotation;
      ScaleDifferencePercent = scale; ExposureDifferenceEv = exposure; ColorDeltaE = color; }
}
public sealed record RigQualityContractV2
{
    public double SeamErrorOutputPixelsMax { get; } public double RegistrationErrorOutputPixelsMax { get; } public double ColorDeltaEMax { get; }
    internal RigQualityContractV2(double seam, double registration, double color)
    { SeamErrorOutputPixelsMax = seam; RegistrationErrorOutputPixelsMax = registration; ColorDeltaEMax = color; }
}
public sealed record RigApprovalV2
{
    public string DecisionRef { get; } public DateTimeOffset ApprovedAt { get; } public DateTimeOffset ValidUntil { get; }
    internal RigApprovalV2(string reference, DateTimeOffset approved, DateTimeOffset until)
    { DecisionRef = reference; ApprovedAt = approved; ValidUntil = until; }
}
public sealed record RigOutputRasterV2
{
    public long LeftUm { get; } public long TopUm { get; } public long RightUm { get; } public long BottomUm { get; }
    public int Dpi { get; } public int WidthPixels { get; } public int HeightPixels { get; }
    public RigOutputRasterV2(long leftUm, long topUm, long rightUm, long bottomUm, int dpi, int widthPixels, int heightPixels)
    {
        LeftUm = leftUm; TopUm = topUm; RightUm = rightUm; BottomUm = bottomUm;
        Dpi = dpi; WidthPixels = widthPixels; HeightPixels = heightPixels;
        Validate();
    }
    public static RigOutputRasterV2 Parse(string json) => RigProfileV2.ParseRaster(json);
    internal void Validate()
    {
        RigProfileV2.Require(LeftUm >= 0 && TopUm >= 0 && RightUm > LeftUm && BottomUm > TopUm, "Invalid raster region.");
        RigProfileV2.Require(Dpi is >= 1 and <= 65535 && WidthPixels is >= 1 and <= 32768 && HeightPixels is >= 1 and <= 32768,
            "Raster dimensions exceed format or engine bounds.");
        RigProfileV2.Require((long)WidthPixels * HeightPixels <= 200_000_000, "Raster pixel count exceeds engine bounds.");
        RigProfileV2.Require(RequiredPixels(RightUm - LeftUm) == WidthPixels && RequiredPixels(BottomUm - TopUm) == HeightPixels,
            "Raster dimensions disagree with exact micrometre/DPI ceiling.");
    }
    private BigInteger RequiredPixels(long extent)
    { var product = (BigInteger)extent * Dpi; return (product + 25399) / 25400; }
}

/// <summary>Immutable rig profile contract. Parsing and validation grant no hardware or quality acceptance.</summary>
public sealed record RigProfileV2
{
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);
    public string SchemaVersion => "2.0.0";
    public string Status { get; } public string? ProfileId { get; }
    public RigDocumentPlaneV2? DocumentPlane { get; } public RigPairV2<RigCameraProjectionV2?> Cameras { get; }
    public RigCalibrationV2? Calibration { get; } public RigOutputRasterV2? OutputRaster { get; }
    public RigCorrectionEnvelopeV2? CorrectionEnvelope { get; } public RigQualityContractV2? QualityContract { get; }
    public RigApprovalV2? Approval { get; }
    public string CanonicalFingerprintText { get; } public string FingerprintSha256 { get; }

    private RigProfileV2(JsonElement root)
    {
        Fields(root, "schemaVersion", "status", "profileId", "conventions", "cameraModel", "documentPlane", "cameras",
            "calibration", "outputRaster", "correctionEnvelope", "qualityContract", "approval");
        Constant(root, "schemaVersion", "2.0.0"); Status = Choice(root.GetProperty("status"), "draft", "approved");
        ProfileId = Null(root.GetProperty("profileId")) ? null : Identifier(root.GetProperty("profileId"));
        var conventions = root.GetProperty("conventions"); Fields(conventions, "documentPlane", "cameraPixel", "matrix", "exifOrientation");
        Constant(conventions, "documentPlane", "mm-origin-sheet-top-left-x-right-y-down");
        Constant(conventions, "cameraPixel", "stored-order-sample-at-integer-index");
        Constant(conventions, "matrix", "row-major-column-vector"); Constant(conventions, "exifOrientation", "ignored");
        var model = root.GetProperty("cameraModel"); Fields(model, "manufacturer", "model", "sensorWidthPixels", "sensorHeightPixels");
        Constant(model, "manufacturer", "Nikon"); Constant(model, "model", "D810");
        Require(Integer(model.GetProperty("sensorWidthPixels")) == 7360 && Integer(model.GetProperty("sensorHeightPixels")) == 4912,
            "Unsupported camera dimensions.");
        var plane = root.GetProperty("documentPlane");
        if (!Null(plane))
        {
            Fields(plane, "paper", "orientation", "sheetWidthMm", "sheetHeightMm", "cameraOrder"); Constant(plane, "paper", "ISO-216-A0");
            var orientation = Choice(plane.GetProperty("orientation"), "landscape", "portrait");
            var width = Number(plane.GetProperty("sheetWidthMm")); var height = Number(plane.GetProperty("sheetHeightMm"));
            Require(width == (orientation == "landscape" ? 1189 : 841) && height == (orientation == "landscape" ? 841 : 1189), "Sheet dimensions disagree with orientation.");
            DocumentPlane = new(orientation, Choice(plane.GetProperty("cameraOrder"), "CAM-A-left-CAM-B-right", "CAM-A-top-CAM-B-bottom"), width, height);
        }
        var cameras = root.GetProperty("cameras"); Fields(cameras, "CAM-A", "CAM-B");
        Cameras = new(ReadCamera(cameras.GetProperty("CAM-A")), ReadCamera(cameras.GetProperty("CAM-B")));
        Calibration = ReadCalibration(root.GetProperty("calibration"));
        OutputRaster = Null(root.GetProperty("outputRaster")) ? null : ReadRaster(root.GetProperty("outputRaster"));
        CorrectionEnvelope = ReadEnvelope(root.GetProperty("correctionEnvelope"));
        var quality = root.GetProperty("qualityContract");
        if (!Null(quality)) { Fields(quality, "seamErrorOutputPixelsMax", "registrationErrorOutputPixelsMax", "colorDeltaEMax");
            QualityContract = new(Nonnegative(quality.GetProperty("seamErrorOutputPixelsMax")), Nonnegative(quality.GetProperty("registrationErrorOutputPixelsMax")), Nonnegative(quality.GetProperty("colorDeltaEMax"))); }
        var approval = root.GetProperty("approval");
        if (!Null(approval)) { Fields(approval, "decisionRef", "approvedAt", "validUntil");
            Approval = new(Identifier(approval.GetProperty("decisionRef")), Timestamp(approval.GetProperty("approvedAt")), Timestamp(approval.GetProperty("validUntil"))); }
        int calibrationBlocks = (DocumentPlane is null ? 0 : 1) + (Cameras.CameraA is null ? 0 : 1) + (Cameras.CameraB is null ? 0 : 1) + (Calibration is null ? 0 : 1);
        Require(calibrationBlocks is 0 or 4, "Calibration blocks must be all-null or all-present.");
        if (Status == "draft") Require(OutputRaster is null && CorrectionEnvelope is null && QualityContract is null && Approval is null, "Draft contains owner decisions.");
        else
        {
            Require(ProfileId is not null && calibrationBlocks == 4 && OutputRaster is not null && CorrectionEnvelope is not null && QualityContract is not null && Approval is not null,
                "Approved profile is incomplete.");
            var measurements = Calibration!.RigMeasurements;
            Require(measurements.BaselineMm.HasValue && measurements.OverlapMm.HasValue && measurements.CameraToDocumentMm.CameraA.HasValue &&
                measurements.CameraToDocumentMm.CameraB.HasValue && measurements.LensFocalLengthMm.CameraA.HasValue && measurements.LensFocalLengthMm.CameraB.HasValue,
                "Approved rig measurements cannot be null.");
            Require(Calibration.MeasuredAt <= Approval!.ApprovedAt && Approval.ApprovedAt < Approval.ValidUntil, "Invalid calibration/approval chronology.");
            ValidateGeometry(OutputRaster!);
        }
        CanonicalFingerprintText = Canonical(root);
        FingerprintSha256 = Convert.ToHexStringLower(SHA256.HashData(StrictUtf8.GetBytes(CanonicalFingerprintText)));
    }

    public static RigProfileV2 Parse(string json)
    {
        using var document = ReadDocument(json);
        return new(document.RootElement);
    }
    internal static RigOutputRasterV2 ParseRaster(string json)
    { using var document = ReadDocument(json); return ReadRaster(document.RootElement); }
    public void ValidateApprovedForUse(DateTimeOffset assessedAt)
    {
        Require(assessedAt.Offset == TimeSpan.Zero, "Assessment must be UTC.");
        Require(Status == "approved" && Calibration is not null && Approval is not null && OutputRaster is not null, "Product use requires approved v2.");
        Require(Calibration!.MeasuredAt <= Approval!.ApprovedAt && Approval.ApprovedAt <= assessedAt && assessedAt < Approval.ValidUntil, "Approval is not valid at assessment time.");
        ValidateGeometry(OutputRaster!);
    }
    public void ValidateCalibrationForEvaluation(RigOutputRasterV2 raster)
    {
        ArgumentNullException.ThrowIfNull(raster);
        Require(Status == "draft" && DocumentPlane is not null && Calibration is not null && Cameras.CameraA is not null && Cameras.CameraB is not null,
            "Evaluation requires a calibrated draft.");
        ValidateGeometry(raster);
    }
    private void ValidateGeometry(RigOutputRasterV2 raster)
    {
        raster.Validate(); Require(DocumentPlane is not null && Cameras.CameraA is not null && Cameras.CameraB is not null, "Missing geometry.");
        Require(raster.RightUm <= DocumentPlane!.SheetWidthMm * 1000 && raster.BottomUm <= DocumentPlane.SheetHeightMm * 1000, "Raster is outside the document sheet.");
        ValidateProjection(Cameras.CameraA!, raster); ValidateProjection(Cameras.CameraB!, raster);
    }

    internal static void Require(bool condition, string message) { if (!condition) throw new InvalidDataException(message); }
    private static bool Null(JsonElement element) => element.ValueKind == JsonValueKind.Null;
    private static JsonDocument ReadDocument(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        Require(json.Length <= 256 * 1024, "Profile text exceeds the resource bound.");
        byte[] bytes;
        try { bytes = StrictUtf8.GetBytes(json); } catch (EncoderFallbackException error) { throw new InvalidDataException("Profile contains invalid Unicode.", error); }
        Require(bytes.Length is > 0 and <= 256 * 1024, "Profile UTF-8 size exceeds the resource bound.");
        JsonDocument document;
        try { document = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 32, AllowTrailingCommas = false, CommentHandling = JsonCommentHandling.Disallow }); }
        catch (JsonException error) { throw new InvalidDataException("Invalid profile JSON.", error); }
        try { Duplicates(document.RootElement); return document; } catch { document.Dispose(); throw; }
    }
    private static void Duplicates(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var field in element.EnumerateObject()) { Require(seen.Add(field.Name), "Duplicate profile field."); Duplicates(field.Value); }
        }
        else if (element.ValueKind == JsonValueKind.Array) foreach (var child in element.EnumerateArray()) Duplicates(child);
    }
    private static void Fields(JsonElement element, params string[] fields)
    {
        Require(element.ValueKind == JsonValueKind.Object, "Expected profile object.");
        var names = element.EnumerateObject().Select(property => property.Name).ToArray();
        Require(names.Length == fields.Length && names.All(name => fields.Contains(name, StringComparer.Ordinal)), "Missing or unknown profile field.");
    }
    private static string Text(JsonElement element) { Require(element.ValueKind == JsonValueKind.String, "Expected profile string."); return element.GetString()!; }
    private static string Choice(JsonElement element, params string[] choices) { var text = Text(element); Require(choices.Contains(text, StringComparer.Ordinal), "Invalid profile enum."); return text; }
    private static void Constant(JsonElement element, string field, string expected) => Require(Text(element.GetProperty(field)) == expected, "Unsupported profile constant.");
    private static string Identifier(JsonElement element)
    {
        var text = Text(element);
        static bool Alnum(char ch) => ch is >= 'a' and <= 'z' or >= 'A' and <= 'Z' or >= '0' and <= '9';
        Require(text.Length is >= 1 and <= 128 && Alnum(text[0]) && text.All(ch => Alnum(ch) || ch is '.' or '_' or '-'), "Invalid ASCII profile identifier."); return text;
    }
    private static double Number(JsonElement element)
    {
        Require(element.ValueKind == JsonValueKind.Number, "Expected profile number.");
        Require(element.TryGetDouble(out var value) && double.IsFinite(value), "Profile number is not finite binary64.");
        return value == 0 ? 0 : value;
    }
    private static double Nonnegative(JsonElement e)
    {
        var value = Number(e); var raw = e.GetRawText(); int exponent = raw.IndexOfAny(['e', 'E']);
        var mantissa = exponent < 0 ? raw : raw[..exponent];
        Require(value >= 0 && !(raw.StartsWith('-') && mantissa.Any(ch => ch is >= '1' and <= '9')), "Negative profile number.");
        return value;
    }
    private static double Positive(JsonElement e) { var value = Number(e); Require(value > 0, "Profile number must be positive."); return value; }
    private static double? OptionalNumber(JsonElement e, bool positive) => Null(e) ? null : positive ? Positive(e) : Nonnegative(e);
    private static int Integer(JsonElement e)
    {
        Require(e.ValueKind == JsonValueKind.Number, "Expected integer."); var raw = e.GetRawText();
        Require(raw.Length > 0 && raw.All(ch => ch is >= '0' and <= '9') && int.TryParse(raw, NumberStyles.None, CultureInfo.InvariantCulture, out _), "Integer requires unsigned integer lexical form.");
        return int.Parse(raw, NumberStyles.None, CultureInfo.InvariantCulture);
    }
    private static DateTimeOffset Timestamp(JsonElement e)
    {
        var text = Text(e); Require(text.Length == 20 && text[19] == 'Z', "UTC timestamp must have whole seconds and Z suffix.");
        for (int index = 0; index < text.Length; index++)
            if (index is not (4 or 7 or 10 or 13 or 16 or 19)) Require(text[index] is >= '0' and <= '9', "Timestamp digits must be ASCII.");
        Require(DateTimeOffset.TryParseExact(text, "yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture,
            DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out var value), "Invalid Gregorian UTC timestamp."); return value;
    }
    private static string Hash(JsonElement e) { var text = Text(e); Require(text.Length == 64 && text.All(ch => ch is >= '0' and <= '9' or >= 'a' and <= 'f'), "Invalid lowercase SHA-256."); return text; }
    private static RigPairV2<T> Pair<T>(JsonElement e, Func<JsonElement, T> read) { Fields(e, "CAM-A", "CAM-B"); return new(read(e.GetProperty("CAM-A")), read(e.GetProperty("CAM-B"))); }
    private static RigCameraProjectionV2? ReadCamera(JsonElement e)
    {
        Fields(e, "physicalIdentity", "projection"); Require(Null(e.GetProperty("physicalIdentity")), "Physical identity is forbidden.");
        var p = e.GetProperty("projection"); if (Null(p)) return null;
        Fields(p, "intrinsics", "distortion", "documentToImage"); var i = p.GetProperty("intrinsics");
        Fields(i, "fxPixels", "fyPixels", "cxPixels", "cyPixels");
        var intrinsics = new RigIntrinsicsV2(Positive(i.GetProperty("fxPixels")), Positive(i.GetProperty("fyPixels")), Number(i.GetProperty("cxPixels")), Number(i.GetProperty("cyPixels")));
        var d = p.GetProperty("distortion"); Fields(d, "model", "k1", "k2", "k3", "p1", "p2"); Constant(d, "model", "brown-conrady-k1k2k3-p1p2");
        var lens = new RigLensDistortionV2(Number(d.GetProperty("k1")), Number(d.GetProperty("k2")), Number(d.GetProperty("k3")), Number(d.GetProperty("p1")), Number(d.GetProperty("p2")));
        var matrix = p.GetProperty("documentToImage"); Require(matrix.ValueKind == JsonValueKind.Array && matrix.GetArrayLength() == 3, "Projection must have three rows.");
        var entries = ImmutableArray.CreateBuilder<double>(9);
        foreach (var row in matrix.EnumerateArray()) { Require(row.ValueKind == JsonValueKind.Array && row.GetArrayLength() == 3, "Projection must have three columns."); foreach (var n in row.EnumerateArray()) entries.Add(Number(n)); }
        var result = new RigCameraProjectionV2(intrinsics, lens, entries.MoveToImmutable()); ValidateHomography(result); return result;
    }
    private static RigCalibrationV2? ReadCalibration(JsonElement e)
    {
        if (Null(e)) return null;
        Fields(e, "method", "chartId", "provenanceId", "measuredAt", "tool", "inputSha256", "fitResidualPixels", "rigMeasurements"); Constant(e, "method", "chart-fiducial-planar");
        var tool = e.GetProperty("tool"); Fields(tool, "toolId", "version");
        var m = e.GetProperty("rigMeasurements"); Fields(m, "baselineMm", "overlapMm", "cameraToDocumentMm", "lensFocalLengthMm");
        var measurements = new RigMeasurementsV2(OptionalNumber(m.GetProperty("baselineMm"), true), OptionalNumber(m.GetProperty("overlapMm"), false),
            Pair(m.GetProperty("cameraToDocumentMm"), n => OptionalNumber(n, true)), Pair(m.GetProperty("lensFocalLengthMm"), n => OptionalNumber(n, true)));
        var residual = Pair(e.GetProperty("fitResidualPixels"), n => { Fields(n, "rms", "max"); return new RigResidualV2(Nonnegative(n.GetProperty("rms")), Nonnegative(n.GetProperty("max"))); });
        return new(Identifier(e.GetProperty("chartId")), Identifier(e.GetProperty("provenanceId")), Timestamp(e.GetProperty("measuredAt")),
            Identifier(tool.GetProperty("toolId")), Identifier(tool.GetProperty("version")), Pair(e.GetProperty("inputSha256"), Hash), residual, measurements);
    }
    private static RigCorrectionEnvelopeV2? ReadEnvelope(JsonElement e)
    {
        if (Null(e)) return null;
        Fields(e, "minimumOverlapMm", "registrationErrorOutputPixels", "rotationErrorDegrees", "scaleDifferencePercent", "exposureDifferenceEv", "colorDeltaE");
        RigEnvelopeMetricV2 Metric(string name) { var n = e.GetProperty(name); Fields(n, "targetMax", "autoCorrectionMax");
            var target = Nonnegative(n.GetProperty("targetMax")); var auto = Nonnegative(n.GetProperty("autoCorrectionMax")); Require(target <= auto, "Correction target exceeds automatic envelope."); return new(target, auto); }
        return new(Nonnegative(e.GetProperty("minimumOverlapMm")), Metric("registrationErrorOutputPixels"), Metric("rotationErrorDegrees"), Metric("scaleDifferencePercent"), Metric("exposureDifferenceEv"), Metric("colorDeltaE"));
    }
    private static long Micrometres(JsonElement e)
    {
        _ = Nonnegative(e); string raw = e.GetRawText(); if (raw.StartsWith('-')) raw = raw[1..];
        int exponentAt = raw.IndexOfAny(['e', 'E']); string mantissa = exponentAt < 0 ? raw : raw[..exponentAt];
        int dot = mantissa.IndexOf('.'); int fractionalDigits = dot < 0 ? 0 : mantissa.Length - dot - 1;
        string digits = mantissa.Replace(".", "", StringComparison.Ordinal).TrimStart('0');
        if (digits.Length == 0) return 0;
        int exponent = 0;
        if (exponentAt >= 0) Require(int.TryParse(raw[(exponentAt + 1)..], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out exponent), "Micrometre exponent is not representable.");
        long shift = (long)exponent - fractionalDigits + 3;
        Require(shift <= 19 && shift >= -digits.Length, "Region is not an exact representable micrometre value.");
        var coefficient = BigInteger.Parse(digits, CultureInfo.InvariantCulture);
        BigInteger value;
        if (shift >= 0) value = coefficient * BigInteger.Pow(10, (int)shift);
        else { value = BigInteger.DivRem(coefficient, BigInteger.Pow(10, checked((int)-shift)), out var remainder); Require(remainder.IsZero, "Region is not an exact micrometre multiple."); }
        Require(value >= 0 && value <= long.MaxValue, "Micrometre value is outside supported range."); return (long)value;
    }
    private static RigOutputRasterV2 ReadRaster(JsonElement e)
    {
        Fields(e, "dpi", "regionMm", "widthPixels", "heightPixels"); var r = e.GetProperty("regionMm"); Fields(r, "left", "top", "right", "bottom");
        return new(Micrometres(r.GetProperty("left")), Micrometres(r.GetProperty("top")), Micrometres(r.GetProperty("right")), Micrometres(r.GetProperty("bottom")),
            Integer(e.GetProperty("dpi")), Integer(e.GetProperty("widthPixels")), Integer(e.GetProperty("heightPixels")));
    }
    private static void ValidateHomography(RigCameraProjectionV2 camera)
    {
        var h = camera.DocumentToImage; Require(h[8] == 1, "Homography requires h22=1.");
        double determinant = h[0] * (h[4] * h[8] - h[5] * h[7]) - h[1] * (h[3] * h[8] - h[5] * h[6]) + h[2] * (h[3] * h[7] - h[4] * h[6]);
        Require(double.IsFinite(determinant) && determinant != 0, "Homography is numerically singular.");
    }
    private static void ValidateProjection(RigCameraProjectionV2 camera, RigOutputRasterV2 raster)
    {
        ValidateHomography(camera); var h = camera.DocumentToImage; var i = camera.Intrinsics;
        double left = raster.LeftUm / 1000.0, top = raster.TopUm / 1000.0, pitch = 25.4 / raster.Dpi;
        double right = Math.Max(raster.RightUm / 1000.0, left + raster.WidthPixels * pitch), bottom = Math.Max(raster.BottomUm / 1000.0, top + raster.HeightPixels * pitch);
        double radius = 0;
        foreach (double xDocument in new[] { left, right }) foreach (double yDocument in new[] { top, bottom })
        {
            double w = h[6] * xDocument + h[7] * yDocument + 1; Require(double.IsFinite(w) && w > 0, "Projective horizon intersects the ceil raster.");
            double x = ((h[0] * xDocument + h[1] * yDocument + h[2]) / w - i.CxPixels) / i.FxPixels;
            double y = ((h[3] * xDocument + h[4] * yDocument + h[5]) / w - i.CyPixels) / i.FyPixels;
            double squared = x * x + y * y; Require(double.IsFinite(squared), "Projection radius is not representable."); radius = Math.Max(radius, squared);
        }
        ValidateRadial(camera.Distortion, radius);
    }
    private static void ValidateRadial(RigLensDistortionV2 lens, double maximum)
    {
        double a = 7 * lens.K3, b = 5 * lens.K2, c = 3 * lens.K1;
        void Evaluate(double t) { double value = ((a * t + b) * t + c) * t + 1; Require(double.IsFinite(value) && value > 0, "Radial mapping folds in the output domain."); }
        Evaluate(0); Evaluate(maximum);
        double qa = 3 * a, qb = 2 * b, qc = c; double scale = Math.Max(Math.Abs(qa), Math.Max(Math.Abs(qb), Math.Abs(qc)));
        if (scale == 0) return; Require(double.IsFinite(scale), "Radial derivative coefficients overflow.");
        qa /= scale; qb /= scale; qc /= scale;
        void At(double t) { if (double.IsFinite(t) && t > 0 && t < maximum) Evaluate(t); }
        if (qa == 0) { if (qb != 0) At(-qc / qb); return; }
        double discriminant = qb * qb - 4 * qa * qc; if (discriminant < 0) return;
        double q = -.5 * (qb + Math.CopySign(Math.Sqrt(discriminant), qb));
        if (q == 0) At(-qb / (2 * qa)); else { At(q / qa); At(qc / q); }
    }

    private static string Canonical(JsonElement root)
    {
        var output = new StringBuilder("a0.rig-profile-fingerprint.v2\n");
        void Emit(JsonElement e, string path)
        {
            if (e.ValueKind == JsonValueKind.Object)
            {
                string key = path.Split('/').Last();
                string[] order = key switch
                {
                    "" => ["schemaVersion","status","profileId","conventions","cameraModel","documentPlane","cameras","calibration","outputRaster","correctionEnvelope","qualityContract","approval"],
                    "conventions" => ["documentPlane","cameraPixel","matrix","exifOrientation"],
                    "cameraModel" => ["manufacturer","model","sensorWidthPixels","sensorHeightPixels"],
                    "documentPlane" => ["paper","orientation","sheetWidthMm","sheetHeightMm","cameraOrder"],
                    "cameras" or "inputSha256" or "fitResidualPixels" or "cameraToDocumentMm" or "lensFocalLengthMm" => ["CAM-A","CAM-B"],
                    "CAM-A" or "CAM-B" when path.StartsWith("/cameras/",StringComparison.Ordinal) => ["physicalIdentity","projection"],
                    "CAM-A" or "CAM-B" => ["rms","max"],
                    "projection" => ["intrinsics","distortion","documentToImage"],
                    "intrinsics" => ["fxPixels","fyPixels","cxPixels","cyPixels"],
                    "distortion" => ["model","k1","k2","k3","p1","p2"],
                    "calibration" => ["method","chartId","provenanceId","measuredAt","tool","inputSha256","fitResidualPixels","rigMeasurements"],
                    "tool" => ["toolId","version"],
                    "rigMeasurements" => ["baselineMm","overlapMm","cameraToDocumentMm","lensFocalLengthMm"],
                    "outputRaster" => ["dpi","regionMm","widthPixels","heightPixels"],
                    "regionMm" => ["left","top","right","bottom"],
                    "correctionEnvelope" => ["minimumOverlapMm","registrationErrorOutputPixels","rotationErrorDegrees","scaleDifferencePercent","exposureDifferenceEv","colorDeltaE"],
                    "qualityContract" => ["seamErrorOutputPixelsMax","registrationErrorOutputPixelsMax","colorDeltaEMax"],
                    "approval" => ["decisionRef","approvedAt","validUntil"],
                    _ => ["targetMax","autoCorrectionMax"]
                };
                foreach (var field in order) Emit(e.GetProperty(field), path + "/" + field);
            }
            else if (e.ValueKind == JsonValueKind.Array) { int index = 0; foreach (var item in e.EnumerateArray()) Emit(item, path + "/" + (index++).ToString(CultureInfo.InvariantCulture)); }
            else
            {
                output.Append(path).Append('=');
                if (Null(e)) output.Append("null");
                else if (e.ValueKind == JsonValueKind.String) output.Append("s:").Append(Text(e));
                else if (path is "/cameraModel/sensorWidthPixels" or "/cameraModel/sensorHeightPixels" or "/outputRaster/dpi" or "/outputRaster/widthPixels" or "/outputRaster/heightPixels")
                    output.Append("i:").Append(Integer(e).ToString(CultureInfo.InvariantCulture));
                else output.Append("f:").Append(BitConverter.DoubleToUInt64Bits(Number(e)).ToString("x16", CultureInfo.InvariantCulture));
                output.Append('\n');
            }
        }
        Emit(root, ""); return output.ToString();
    }
}
