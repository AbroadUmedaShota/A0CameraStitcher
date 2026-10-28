<#
.SYNOPSIS
  Checks the detector chart export (Export-DetectorChartPdf.ps1) and, with -DetectorExecutable,
  that the fiducial detector reads a simulated capture of the generated chart. Issue #371.

.DESCRIPTION
  Part 1, the export (needs Google Chrome or Microsoft Edge):
    - A3 portrait (default), A4 landscape and A0 portrait: exit 0, PDF of one page within 0.5 mm of
      the paper, no image XObject; the chart definition agrees with the paper, margin and pitch, with
      the SVG (size, ring count, last point id, SHA-256) and with the job template;
    - the default A3 SVG is byte for byte the committed master in samples/public/detector-chart,
      and a second run gives the same SVG;
    - refusals: output inside the repository, an existing output file, a margin too small for the
      labels.
  Part 2, the detection (needs -DetectorExecutable, the Release build of
  A0CameraStitcher.FiducialDetector with A0_BUILD_FIDUCIAL_DETECTOR=ON):
    - the A3 SVG is rasterized by the browser at 18 px/mm;
    - two captures are simulated from it: a pinhole camera (f = 9 px/mm x 1140 mm) looks at the
      chart tilted (25 deg / -12 deg, rolled 3 deg; training) and flat (rolled -2 deg; validation),
      about 9 px/mm at the chart centre, 3 x 3 supersampling per pixel, Gaussian blur sigma 0.9 px,
      a brightness slope (gain 0.6 to 1.0 across the frame), noise sigma 1.5 and JPEG quality 90;
    - the job uses the generated chart definition and three anchors per view: the projected node
      centres rounded to a multiple of 12 px with a search radius of 40 px (the operator's role,
      as in tests/fiducial_detector_tests.cpp);
    - the detector must exit 0 and write session.json; every view must be "ok" with a verified
      identity; the detected centres are compared with the projected node centres.
  The tolerances below are inputs of this test, not quality thresholds of the product or the optics.
  All files are written to a new folder outside the repository; it is removed on success unless
  -KeepWork is given.

  Exit 0 = all checks passed, 1 = a check failed, 2 = could not run.
#>
#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$DetectorExecutable,
    [string]$BrowserPath,
    [string]$WorkDirectory,
    [switch]$KeepWork,
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$invariant = [Globalization.CultureInfo]::InvariantCulture
$root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
$export = Join-Path $root 'scripts/Export-DetectorChartPdf.ps1'
$masterPath = Join-Path $root 'samples/public/detector-chart/a0cs-detector-chart-a3-portrait-p20-m20-v1.svg'
$pwsh = (Get-Process -Id $PID).Path
$failures = [Collections.Generic.List[string]]::new()
$checks = 0

# Test inputs (not product thresholds).
$sourcePxPerMm = 18
$maxCentreErrorPx = 1.0
$maxRmsCentreErrorPx = 0.5

function Check([bool]$Condition, [string]$Label) {
    $script:checks++
    if ($Condition) { Write-Host "  ok   $Label" } else { Write-Host "  FAIL $Label"; $script:failures.Add($Label) }
}

function Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

function Find-Browser {
    if ($BrowserPath) { return $BrowserPath }
    foreach ($candidate in @(
            (Join-Path $env:ProgramFiles 'Google/Chrome/Application/chrome.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'Google/Chrome/Application/chrome.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'Microsoft/Edge/Application/msedge.exe'),
            (Join-Path $env:ProgramFiles 'Microsoft/Edge/Application/msedge.exe'))) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { return $candidate }
    }
    return $null
}

function Invoke-Export([string[]]$Arguments) {
    $all = @('-NoProfile', '-File', $export) + $Arguments
    if ($BrowserPath) { $all += @('-BrowserPath', $BrowserPath) }
    $output = & $pwsh @all 2>&1 | Out-String
    return @{ ExitCode = $LASTEXITCODE; Output = $output }
}

function Read-PdfFacts([string]$Path) {
    $text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($Path))
    $box = [regex]::Match($text, '/MediaBox\s*\[\s*([-0-9.]+)\s+([-0-9.]+)\s+([-0-9.]+)\s+([-0-9.]+)\s*\]')
    $mm = 25.4 / 72
    return @{
        Pages    = [regex]::Matches($text, '/Type\s*/Page(?![a-zA-Z])').Count
        WidthMm  = if ($box.Success) { ([double]::Parse($box.Groups[3].Value, $invariant) - [double]::Parse($box.Groups[1].Value, $invariant)) * $mm } else { -1 }
        HeightMm = if ($box.Success) { ([double]::Parse($box.Groups[4].Value, $invariant) - [double]::Parse($box.Groups[2].Value, $invariant)) * $mm } else { -1 }
        HasImage = $text -match '/Subtype\s*/Image'
    }
}

function Test-ExportCase([string]$Paper, [string]$Orientation, [string]$Folder) {
    New-Item -ItemType Directory -Path $Folder | Out-Null
    $result = Invoke-Export @('-Paper', $Paper, '-Orientation', $Orientation, '-OutputDirectory', $Folder)
    Check ($result.ExitCode -eq 0) "$Paper $Orientation export exits 0"
    if ($result.ExitCode -ne 0) { Write-Host $result.Output; return $null }
    $id = 'a0cs-detector-chart-{0}-{1}-p20-m20-v1' -f $Paper.ToLowerInvariant(), $Orientation
    $base = Join-Path $Folder $id
    foreach ($suffix in @('.svg', '.pdf', '.chart.json', '.detector-job.template.json')) {
        Check (Test-Path -LiteralPath "$base$suffix" -PathType Leaf) "$Paper $Orientation writes $id$suffix"
    }
    $paperSize = @{ A4 = @(210, 297); A3 = @(297, 420); A0 = @(841, 1189) }[$Paper]
    $width = if ($Orientation -eq 'portrait') { $paperSize[0] } else { $paperSize[1] }
    $height = if ($Orientation -eq 'portrait') { $paperSize[1] } else { $paperSize[0] }

    $pdf = Read-PdfFacts "$base.pdf"
    Check ($pdf.Pages -eq 1) "$Paper PDF has one page"
    Check ([math]::Abs($pdf.WidthMm - $width) -le 0.5 -and [math]::Abs($pdf.HeightMm - $height) -le 0.5) ("$Paper PDF page {0:N2} x {1:N2} mm is within 0.5 mm of {2} x {3}" -f $pdf.WidthMm, $pdf.HeightMm, $width, $height)
    Check (-not $pdf.HasImage) "$Paper PDF has no image XObject"
    Check ($result.Output -match [regex]::Escape((Sha256 "$base.pdf"))) "$Paper export prints the PDF SHA-256"

    $definition = Get-Content -LiteralPath "$base.chart.json" -Raw | ConvertFrom-Json
    $chart = $definition.chart
    $columns = [int][math]::Floor(($width - 40) / 20); $rows = [int][math]::Floor(($height - 40) / 20)
    Check ($definition.chartId -ceq $id -and $definition.paper.widthMm -eq $width -and $definition.paper.heightMm -eq $height) "$Paper chart definition names the paper and the id"
    Check ($chart.columns -eq $columns -and $chart.rows -eq $rows -and $chart.pitchMm -eq 20) "$Paper lattice is $columns x $rows cells at pitch 20"
    Check ([math]::Abs($chart.originXMm - ($width - $columns * 20) / 2) -lt 1e-9 -and [math]::Abs($chart.originYMm - ($height - $rows * 20) / 2) -lt 1e-9) "$Paper lattice is centred on the sheet"
    Check ([math]::Abs($definition.design.ringOuterRadiusMm - 2.8) -lt 1e-9 -and [math]::Abs($definition.design.ringWhiteRadiusMm - 1.68) -lt 1e-9 -and [math]::Abs($definition.design.ringDotRadiusMm - 0.7) -lt 1e-9) "$Paper ring radii are 2.8 / 1.68 / 0.7 mm"
    Check ($definition.svgSha256 -ceq (Sha256 "$base.svg")) "$Paper chart definition records the SVG SHA-256"
    Check ($result.Output -match [regex]::Escape($definition.svgSha256)) "$Paper export prints the SVG SHA-256"

    $svg = Get-Content -LiteralPath "$base.svg" -Raw
    Check ($svg -match ('width="{0}mm" height="{1}mm" viewBox="0 0 {0} {1}"' -f $width, $height)) "$Paper SVG is $width x $height mm, 1 unit = 1 mm"
    $rings = [regex]::Matches($svg, 'xlink:href="#ring"').Count
    Check ($rings -eq ($columns + 1) * ($rows + 1) -and $rings -eq $definition.nodes.count) "$Paper SVG has a ring on each of the $rings nodes"
    Check ($svg.Contains(('data-point-id="{0}"' -f $definition.nodes.lastPointId)) -and $definition.nodes.lastPointId -ceq ('F-{0:d3}-{1:d3}' -f $columns, $rows)) "$Paper last point id is $($definition.nodes.lastPointId)"
    $gray = 0; $patterns = @{}
    for ($r = 0; $r -lt $rows; $r++) { for ($c = 0; $c -lt $columns; $c++) { $k = (3 * $c + 5 * $r) % 8; $patterns[$k] = 1 + [int]$patterns[$k]; if ($k -eq 5) { $gray++ } } }
    Check ($definition.design.grayCells -eq $gray -and $gray -ge 2) "$Paper has $gray grey cells"
    Check ([regex]::Matches($svg, '#edge-dark-on-light"').Count -eq [int]$patterns[1] -and [regex]::Matches($svg, '#edge-light-on-dark"').Count -eq [int]$patterns[7] `
            -and [regex]::Matches($svg, '#fine-lines-v"').Count -eq [int]$patterns[3] -and [regex]::Matches($svg, '#fine-lines-h"').Count -eq [int]$patterns[4]) "$Paper cell patterns follow (3c + 5r) mod 8"
    Check ($svg.Contains('100 mm') -and $svg.Contains('id="scale-100mm"')) "$Paper has the 100 mm scales"

    $job = Get-Content -LiteralPath "$base.detector-job.template.json" -Raw | ConvertFrom-Json
    Check ($job.schema -ceq 'a0.fiducial-detector-job/1' -and $job.chartId -ceq $id) "$Paper job template schema and chart id"
    Check (($job.chart | ConvertTo-Json -Compress) -ceq ($chart | ConvertTo-Json -Compress)) "$Paper job template chart equals the chart definition"
    $views = @($job.views)
    $counts = foreach ($camera in 'CAM-A', 'CAM-B') {
        foreach ($role in 'training', 'validation', 'reference') { @($views | Where-Object { $_.camera -eq $camera -and $_.role -eq $role }).Count }
    }
    Check ($views.Count -eq 18 -and (($counts -join ',') -eq '6,2,1,6,2,1')) "$Paper job template has 6 training, 2 validation and 1 reference view per camera"
    return @{ Base = $base; Definition = $definition }
}

# ---- capture simulator ----------------------------------------------------------------------
$simulator = @'
using System;
public static class DetectorChartCaptureSimulator {
    // src: 24 bpp BGR rows of the rasterized chart (srcPxPerMm, pixel i covers [i, i+1) / ppm mm).
    // inverse: 3 x 3 row-major map from image pixel (x, y, 1) to chart mm (homogeneous).
    // Returns 24 bpp BGR rows (grey) of the simulated capture.
    public static byte[] Render(byte[] src, int srcW, int srcH, int srcStride, double ppm, double sheetW, double sheetH,
        double[] inverse, int outW, int outH, int outStride, int supersample, double blurSigma,
        double gain0, double gainX, double gainY, double tableLevel, double noiseSigma, int seed) {
        var gray = new float[srcW * srcH];
        for (int y = 0; y < srcH; y++) {
            int o = y * srcStride;
            for (int x = 0; x < srcW; x++) {
                gray[y * srcW + x] = (float)(0.114 * src[o + 3 * x] + 0.587 * src[o + 3 * x + 1] + 0.299 * src[o + 3 * x + 2]);
            }
        }
        var img = new double[outW * outH];
        for (int y = 0; y < outH; y++) {
            for (int x = 0; x < outW; x++) {
                double sum = 0;
                for (int j = 0; j < supersample; j++) {
                    double v = y - 0.5 + (j + 0.5) / supersample;
                    for (int i = 0; i < supersample; i++) {
                        double u = x - 0.5 + (i + 0.5) / supersample;
                        double w = inverse[6] * u + inverse[7] * v + inverse[8];
                        double mx = (inverse[0] * u + inverse[1] * v + inverse[2]) / w;
                        double my = (inverse[3] * u + inverse[4] * v + inverse[5]) / w;
                        if (!(mx >= 0 && my >= 0 && mx < sheetW && my < sheetH)) { sum += tableLevel; continue; }
                        double sx = mx * ppm - 0.5, sy = my * ppm - 0.5;
                        int x0 = (int)Math.Floor(sx), y0 = (int)Math.Floor(sy);
                        double fx = sx - x0, fy = sy - y0;
                        int xa = Math.Clamp(x0, 0, srcW - 1), xb = Math.Clamp(x0 + 1, 0, srcW - 1);
                        int ya = Math.Clamp(y0, 0, srcH - 1), yb = Math.Clamp(y0 + 1, 0, srcH - 1);
                        sum += (1 - fy) * ((1 - fx) * gray[ya * srcW + xa] + fx * gray[ya * srcW + xb])
                             + fy * ((1 - fx) * gray[yb * srcW + xa] + fx * gray[yb * srcW + xb]);
                    }
                }
                img[y * outW + x] = sum / (supersample * supersample);
            }
        }
        int radius = (int)Math.Ceiling(3 * blurSigma);
        var kernel = new double[2 * radius + 1];
        double ks = 0;
        for (int k = -radius; k <= radius; k++) { kernel[k + radius] = Math.Exp(-0.5 * k * k / (blurSigma * blurSigma)); ks += kernel[k + radius]; }
        for (int k = 0; k < kernel.Length; k++) kernel[k] /= ks;
        var tmp = new double[outW * outH];
        for (int y = 0; y < outH; y++)
            for (int x = 0; x < outW; x++) {
                double s = 0;
                for (int k = -radius; k <= radius; k++) s += kernel[k + radius] * img[y * outW + Math.Clamp(x + k, 0, outW - 1)];
                tmp[y * outW + x] = s;
            }
        var rng = new Random(seed);
        var result = new byte[outStride * outH];
        for (int y = 0; y < outH; y++)
            for (int x = 0; x < outW; x++) {
                double s = 0;
                for (int k = -radius; k <= radius; k++) s += kernel[k + radius] * tmp[Math.Clamp(y + k, 0, outH - 1) * outW + x];
                double gain = gain0 + gainX * x / (outW - 1.0) + gainY * y / (outH - 1.0);
                double n = Math.Sqrt(-2.0 * Math.Log(1.0 - rng.NextDouble())) * Math.Cos(2.0 * Math.PI * rng.NextDouble());
                byte b = (byte)Math.Clamp(Math.Round(s * gain + noiseSigma * n), 0, 255);
                int o = y * outStride + 3 * x;
                result[o] = b; result[o + 1] = b; result[o + 2] = b;
            }
        return result;
    }
}
'@

# 3 x 3 matrices are row-major [double[]] of 9 values.
function Mul3([double[]]$P, [double[]]$Q) {
    $m = [double[]]::new(9)
    for ($i = 0; $i -lt 3; $i++) {
        for ($j = 0; $j -lt 3; $j++) {
            $s = 0.0
            for ($k = 0; $k -lt 3; $k++) { $s += $P[3 * $i + $k] * $Q[3 * $k + $j] }
            $m[3 * $i + $j] = $s
        }
    }
    return , $m
}

# Chart mm -> image pixel of a pinhole camera at Distance mm in front of the chart centre, the chart
# rotated by Rz(Roll) Rx(TiltX) Ry(TiltY). An integer pixel index is a pixel centre.
function Get-Homography([double]$SheetW, [double]$SheetH, [double]$TiltX, [double]$TiltY, [double]$Roll, [double]$Focal, [double]$Distance, [int]$OutW, [int]$OutH) {
    $d2r = [math]::PI / 180
    $ca = [math]::Cos($TiltX * $d2r); $sa = [math]::Sin($TiltX * $d2r)
    $cb = [math]::Cos($TiltY * $d2r); $sb = [math]::Sin($TiltY * $d2r)
    $cg = [math]::Cos($Roll * $d2r); $sg = [math]::Sin($Roll * $d2r)
    [double[]]$rx = 1, 0, 0, 0, $ca, (-$sa), 0, $sa, $ca
    [double[]]$ry = $cb, 0, $sb, 0, 1, 0, (-$sb), 0, $cb
    [double[]]$rz = $cg, (-$sg), 0, $sg, $cg, 0, 0, 0, 1
    $r = Mul3 (Mul3 $rz $rx) $ry
    [double[]]$k = $Focal, 0, (($OutW - 1) / 2), 0, $Focal, (($OutH - 1) / 2), 0, 0, 1
    [double[]]$rt = $r[0], $r[1], 0, $r[3], $r[4], 0, $r[6], $r[7], $Distance
    [double[]]$shift = 1, 0, (-$SheetW / 2), 0, 1, (-$SheetH / 2), 0, 0, 1
    return , (Mul3 (Mul3 $k $rt) $shift)
}

function Invert3([double[]]$M) {
    $a = $M[0]; $b = $M[1]; $c = $M[2]; $d = $M[3]; $e = $M[4]; $f = $M[5]; $g = $M[6]; $h = $M[7]; $i = $M[8]
    $det = $a * ($e * $i - $f * $h) - $b * ($d * $i - $f * $g) + $c * ($d * $h - $e * $g)
    [double[]]$inverse = (($e * $i - $f * $h) / $det), (($c * $h - $b * $i) / $det), (($b * $f - $c * $e) / $det),
        (($f * $g - $d * $i) / $det), (($a * $i - $c * $g) / $det), (($c * $d - $a * $f) / $det),
        (($d * $h - $e * $g) / $det), (($b * $g - $a * $h) / $det), (($a * $e - $b * $d) / $det)
    return , $inverse
}

function Project([double[]]$H, [double]$X, [double]$Y) {
    $w = $H[6] * $X + $H[7] * $Y + $H[8]
    return , [double[]]@((($H[0] * $X + $H[1] * $Y + $H[2]) / $w), (($H[3] * $X + $H[4] * $Y + $H[5]) / $w))
}

# ---- run ---------------------------------------------------------------------------------------
$exitCode = 1
$work = $null
$created = $false
try {
    $browser = Find-Browser
    if (-not $browser) { Write-Host 'No Google Chrome or Microsoft Edge was found. Pass -BrowserPath.'; exit 2 }
    if ($DetectorExecutable -and -not (Test-Path -LiteralPath $DetectorExecutable -PathType Leaf)) { Write-Host 'DetectorExecutable was not found.'; exit 2 }
    $candidate = if ($WorkDirectory) { $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($WorkDirectory) } else { Join-Path ([IO.Path]::GetTempPath()) ('a0-detector-chart-test-' + [Guid]::NewGuid().ToString('N')) }
    if ($candidate.TrimEnd([IO.Path]::DirectorySeparatorChar) -ieq $root -or $candidate.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { Write-Host 'WorkDirectory must be outside the repository.'; exit 2 }
    if (Test-Path -LiteralPath $candidate) { Write-Host 'WorkDirectory must not exist yet.'; exit 2 }
    # Only a folder this run created is ever removed in finally.
    New-Item -ItemType Directory -Path $candidate | Out-Null
    $work = $candidate
    $created = $true

    Write-Host 'Export'
    $a3 = Test-ExportCase 'A3' 'portrait' (Join-Path $work 'a3')
    $null = Test-ExportCase 'A4' 'landscape' (Join-Path $work 'a4')
    $null = Test-ExportCase 'A0' 'portrait' (Join-Path $work 'a0')
    if ($a3) {
        Check ((Test-Path -LiteralPath $masterPath -PathType Leaf) -and ((Sha256 $masterPath) -ceq (Sha256 "$($a3.Base).svg"))) 'default A3 SVG equals the committed master'
        $again = Join-Path $work 'a3-again'
        New-Item -ItemType Directory -Path $again | Out-Null
        $second = Invoke-Export @('-SvgOnly', '-OutputDirectory', $again)
        Check ($second.ExitCode -eq 0 -and (Sha256 (Join-Path $again 'a0cs-detector-chart-a3-portrait-p20-m20-v1.svg')) -ceq (Sha256 "$($a3.Base).svg")) 'a second run writes the same SVG'
        $repeat = Invoke-Export @('-SvgOnly', '-OutputDirectory', $again)
        Check ($repeat.ExitCode -eq 1 -and $repeat.Output -match 'not overwritten') 'existing outputs are not overwritten'
    }
    $inside = Invoke-Export @('-SvgOnly', '-OutputDirectory', (Join-Path $root 'samples'))
    Check ($inside.ExitCode -eq 1 -and $inside.Output -match 'inside the repository') 'output inside the repository is refused'
    $narrowFolder = Join-Path $work 'narrow'
    New-Item -ItemType Directory -Path $narrowFolder | Out-Null
    $narrow = Invoke-Export @('-SvgOnly', '-PitchMm', '30', '-MarginMm', '15', '-OutputDirectory', $narrowFolder)
    Check ($narrow.ExitCode -eq 1 -and $narrow.Output -match 'free band') 'a margin too small for the labels is refused'

    if (-not $DetectorExecutable) {
        Write-Host 'Detection: not run (pass -DetectorExecutable)'
    }
    elseif ($a3) {
        Write-Host 'Detection'
        $definition = $a3.Definition
        $sheetW = [double]$definition.paper.widthMm; $sheetH = [double]$definition.paper.heightMm
        $srcW = [int][math]::Round($sheetW * $sourcePxPerMm); $srcH = [int][math]::Round($sheetH * $sourcePxPerMm)
        $raster = Join-Path $work 'raster'
        New-Item -ItemType Directory -Path $raster | Out-Null
        $svgText = Get-Content -LiteralPath "$($a3.Base).svg" -Raw
        $inline = $svgText.Substring($svgText.IndexOf('<svg', [StringComparison]::Ordinal))
        $html = "<!doctype html><html><head><meta charset=`"utf-8`"><style>html,body{margin:0;padding:0;overflow:hidden;background:#fff}svg{display:block;width:${srcW}px;height:${srcH}px}</style></head><body>$inline</body></html>"
        $htmlPath = Join-Path $raster 'raster.html'
        Set-Content -LiteralPath $htmlPath -Value $html -Encoding utf8NoBOM
        $png = Join-Path $raster 'chart.png'
        $arguments = @('--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run', '--no-default-browser-check', "--user-data-dir=`"$(Join-Path $raster 'profile')`"",
            "--window-size=$srcW,$srcH", '--force-device-scale-factor=1', "--screenshot=`"$png`"", ([Uri]$htmlPath).AbsoluteUri)
        $process = Start-Process -FilePath $browser -ArgumentList $arguments -PassThru -WindowStyle Hidden
        if (-not $process.WaitForExit(180000)) { try { $process.Kill($true) } catch { } }
        Check (Test-Path -LiteralPath $png -PathType Leaf) "the browser rasterized the A3 SVG at $sourcePxPerMm px/mm"

        Add-Type -AssemblyName System.Drawing
        Add-Type -TypeDefinition $simulator -Language CSharp
        $bitmap = [Drawing.Bitmap]::new($png)
        try {
            Check ($bitmap.Width -eq $srcW -and $bitmap.Height -eq $srcH) "raster is $srcW x $srcH px"
            $rect = [Drawing.Rectangle]::new(0, 0, $bitmap.Width, $bitmap.Height)
            $data = $bitmap.LockBits($rect, [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
            $srcStride = $data.Stride
            $srcBytes = [byte[]]::new($srcStride * $bitmap.Height)
            [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $srcBytes, 0, $srcBytes.Length)
            $bitmap.UnlockBits($data)
        } finally { $bitmap.Dispose() }

        $outW = 3600; $outH = 4800; $distance = 1140.0; $focal = 9.0 * $distance
        $views = @(
            @{ Id = 'CAM-A-training-01'; Camera = 'CAM-A'; Role = 'training'; TiltX = 25.0; TiltY = -12.0; Roll = 3.0; Seed = 371 },
            @{ Id = 'CAM-B-validation-01'; Camera = 'CAM-B'; Role = 'validation'; TiltX = 0.0; TiltY = 0.0; Roll = -2.0; Seed = 372 }
        )
        $chart = $definition.chart
        $c0 = [int][math]::Floor($chart.columns / 2) - 1; $r0 = [int][math]::Floor($chart.rows / 2) - 1
        $jobViews = [Collections.Generic.List[object]]::new()
        $encoder = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
        $encoderParameters = [Drawing.Imaging.EncoderParameters]::new(1)
        $encoderParameters.Param[0] = [Drawing.Imaging.EncoderParameter]::new([Drawing.Imaging.Encoder]::Quality, [long]90)
        foreach ($view in $views) {
            $h = Get-Homography $sheetW $sheetH $view.TiltX $view.TiltY $view.Roll $focal $distance $outW $outH
            $view.H = $h
            $centre = Project $h ($sheetW / 2) ($sheetH / 2)
            $right = Project $h ($sheetW / 2 + 1) ($sheetH / 2)
            $view.PxPerMm = [math]::Sqrt([math]::Pow($right[0] - $centre[0], 2) + [math]::Pow($right[1] - $centre[1], 2))
            $outStride = (3 * $outW + 3) -band -4
            $bytes = [DetectorChartCaptureSimulator]::Render($srcBytes, $srcW, $srcH, $srcStride, $sourcePxPerMm, $sheetW, $sheetH,
                (Invert3 $h), $outW, $outH, $outStride, 3, 0.9, 0.6, 0.3, 0.1, 0.18 * 255, 1.5, $view.Seed)
            $image = [Drawing.Bitmap]::new($outW, $outH, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
            try {
                $lock = $image.LockBits([Drawing.Rectangle]::new(0, 0, $outW, $outH), [Drawing.Imaging.ImageLockMode]::WriteOnly, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
                [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $lock.Scan0, $bytes.Length)
                $image.UnlockBits($lock)
                New-Item -ItemType Directory -Path (Join-Path $raster $view.Camera) -Force | Out-Null
                $image.Save((Join-Path $raster "$($view.Camera)/$($view.Id).jpg"), $encoder, $encoderParameters)
            } finally { $image.Dispose() }
            $anchors = foreach ($node in @(@($c0, $r0), @(($c0 + 2), $r0), @($c0, ($r0 + 2)))) {
                $p = Project $h ($chart.originXMm + $node[0] * $chart.pitchMm) ($chart.originYMm + $node[1] * $chart.pitchMm)
                [ordered]@{ column = $node[0]; row = $node[1]; xPx = 12 * [math]::Round($p[0] / 12); yPx = 12 * [math]::Round($p[1] / 12); radiusPx = 40 }
            }
            $jobViews.Add([ordered]@{ viewId = $view.Id; camera = $view.Camera; role = $view.Role; image = "$($view.Camera)/$($view.Id).jpg"; anchors = @($anchors) })
            Write-Host ('  view {0}: {1:N2} px/mm at the chart centre' -f $view.Id, $view.PxPerMm)
        }
        $camera = [ordered]@{ widthPixels = $outW; heightPixels = $outH; lensLabel = 'simulated'; focalLengthLabel = 'simulated'; focusLabel = 'simulated' }
        $job = [ordered]@{
            schema = 'a0.fiducial-detector-job/1'; sessionId = 'detector-chart-simulated'; chartId = $definition.chartId; measuredAtUtc = '2000-01-01T00:00:00Z'
            chart = $chart; cameras = [ordered]@{ 'CAM-A' = $camera; 'CAM-B' = $camera }; views = $jobViews
        }
        $jobPath = Join-Path $raster 'detection-job.json'
        Set-Content -LiteralPath $jobPath -Value ($job | ConvertTo-Json -Depth 8) -Encoding utf8NoBOM
        $detectorOut = Join-Path $work 'detector-output'
        & $DetectorExecutable --job $jobPath --output $detectorOut | Out-Null
        $detectorExit = $LASTEXITCODE
        Check ($detectorExit -eq 0) "the detector exits 0 (exit $detectorExit)"
        Check (Test-Path -LiteralPath (Join-Path $detectorOut 'session.json') -PathType Leaf) 'the detector writes session.json'
        $reportPath = Join-Path $detectorOut 'detection-report.json'
        if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
            $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
            foreach ($entry in @($report.views)) {
                Write-Host ('  {0}: status {1}, detected {2}, missing {3}, ambiguous {4}, identity {5} ({6} cells, {7} grey)' -f $entry.viewId, $entry.status, $entry.detected, $entry.missing, $entry.ambiguous, $entry.identity.result, $entry.identity.cellsExamined, $entry.identity.grayCellsExamined)
                Check ($entry.status -eq 'ok' -and $entry.identity.result -eq 'verified') "$($entry.viewId) is ok with a verified identity"
                Check ($entry.detected -eq $definition.nodes.count) "$($entry.viewId) detects all $($definition.nodes.count) nodes"
            }
        }
        $sessionPath = Join-Path $detectorOut 'session.json'
        if (Test-Path -LiteralPath $sessionPath -PathType Leaf) {
            $session = Get-Content -LiteralPath $sessionPath -Raw | ConvertFrom-Json
            foreach ($view in $views) {
                $observed = @($session.views | Where-Object { $_.viewId -eq $view.Id })
                $sum = 0.0; $max = 0.0; $n = 0
                foreach ($o in @($observed[0].observations | Where-Object { $_.state -eq 'detected' })) {
                    $parts = $o.pointId.Split('-')
                    $t = Project $view.H ($chart.originXMm + [int]$parts[1] * $chart.pitchMm) ($chart.originYMm + [int]$parts[2] * $chart.pitchMm)
                    $e = [math]::Sqrt([math]::Pow($o.pixel.x - $t[0], 2) + [math]::Pow($o.pixel.y - $t[1], 2))
                    $sum += $e * $e; $max = [math]::Max($max, $e); $n++
                }
                $rms = if ($n -gt 0) { [math]::Sqrt($sum / $n) } else { [double]::NaN }
                Write-Host ('  {0}: {1} centres, error to the projected node rms {2:N3} px, max {3:N3} px' -f $view.Id, $n, $rms, $max)
                Check ($n -gt 0 -and $rms -le $maxRmsCentreErrorPx -and $max -le $maxCentreErrorPx) "$($view.Id) centres are within rms $maxRmsCentreErrorPx / max $maxCentreErrorPx px of the projected nodes (test input)"
            }
        }
    }

    Write-Host ''
    if ($failures.Count -eq 0) {
        Write-Host "All $checks checks passed."
        $exitCode = 0
    } else {
        Write-Host "$($failures.Count) of $checks checks failed. Work folder kept for inspection."
        $KeepWork = $true
    }
}
catch {
    Write-Host "Could not run: $($_.Exception.Message)"
    $exitCode = 2
    $KeepWork = $true
}
finally {
    if ($created -and (Test-Path -LiteralPath $work)) {
        if ($KeepWork) { Write-Host "Work folder: $work" } else { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
    }
}
exit $exitCode
