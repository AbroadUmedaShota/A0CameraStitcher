<#
.SYNOPSIS
  Converts a pilot chart print master (SVG) into a one-page A0 PDF for printing. Issue #276.

.DESCRIPTION
  The SVG master in samples/public/pilot-chart is the source of truth; the PDF is a derived file
  and is not committed (its bytes change with the browser version and the creation date).
  The script:
    1. checks the master against its vector spec (SHA-256 of the SVG),
    2. prints it with a locally installed Chromium browser (Microsoft Edge or Google Chrome) in
       headless mode through a wrapper page with @page size 841mm x 1189mm and no margins,
    3. checks the PDF: one page, page size A0 within 0.5 mm, and no image XObject in the
       uncompressed object dictionaries (the chart stays vector),
    4. prints the SHA-256 of the master and of the PDF, so the printed copy can be traced back.
  The output must be outside the repository. No camera, SDK or network is used.

.PARAMETER Chart
  development or holdout.

.PARAMETER OutputPath
  Path of the PDF to write. Must not be inside the repository.

.PARAMETER BrowserPath
  Optional path to msedge.exe or chrome.exe. Default: the standard install locations.

.EXAMPLE
  pwsh -NoProfile -File scripts/Export-PilotChartPdf.ps1 -Chart development -OutputPath <folder outside the repository>\a0-pilot-chart-development-v1.pdf
#>
#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('development', 'holdout')][string]$Chart,
    [Parameter(Mandatory)][string]$OutputPath,
    [string]$BrowserPath,
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
$pointsPerMm = 72 / 25.4
$toleranceMm = 0.5

function Fail([string]$Message) {
    Write-Error $Message -ErrorAction Continue
    exit 1
}

try {
    $root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $output = [IO.Path]::GetFullPath($OutputPath)
    if ($output.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        Fail 'OutputPath is inside the repository. Write the PDF to a folder outside the repository.'
    }
    if ([IO.Path]::GetExtension($output) -ne '.pdf') { Fail 'OutputPath must end with .pdf.' }
    $outputDirectory = Split-Path -Parent $output
    if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) { Fail 'The folder of OutputPath does not exist.' }

    $specPath = Join-Path $root "samples/public/corpus-contracts/vector-specs/pilot-chart-$Chart-v1.json"
    $spec = Get-Content -LiteralPath $specPath -Raw | ConvertFrom-Json
    $masterPath = Join-Path $root $spec.printMaster.masterPath
    $masterHash = (Get-FileHash -LiteralPath $masterPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($masterHash -cne [string]$spec.printMaster.masterSha256) { Fail 'The SVG master does not match the hash in its vector spec. Do not print it.' }

    # Chrome first: on some PCs Edge exits without writing the PDF (seen 2026-10-08), so every
    # installed browser is tried in turn until one writes a PDF.
    $browsers = if ($BrowserPath) { @($BrowserPath) } else {
        @(
            (Join-Path $env:ProgramFiles 'Google/Chrome/Application/chrome.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'Google/Chrome/Application/chrome.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'Microsoft/Edge/Application/msedge.exe'),
            (Join-Path $env:ProgramFiles 'Microsoft/Edge/Application/msedge.exe')
        )
    }
    $browsers = @($browsers | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) })
    if ($browsers.Count -eq 0) { Fail 'No Google Chrome or Microsoft Edge was found. Pass -BrowserPath.' }

    $work = Join-Path ([IO.Path]::GetTempPath()) ("a0-pilot-chart-pdf-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work | Out-Null
    try {
        # The SVG is inlined: an SVG loaded through <img> is rasterized by the browser when it
        # prints (seen 2026-10-08: one 862 x 1218 pixel bitmap for the whole sheet).
        $svgText = Get-Content -LiteralPath $masterPath -Raw
        $html = @'
<!doctype html>
<html><head><meta charset="utf-8"><style>
@page { size: 841mm 1189mm; margin: 0; }
html, body { margin: 0; padding: 0; }
svg { display: block; width: 841mm; height: 1189mm; }
</style></head>
<body>
'@ + $svgText + @'
</body></html>
'@
        $htmlPath = Join-Path $work 'print.html'
        Set-Content -LiteralPath $htmlPath -Value $html -Encoding utf8NoBOM
        $tempPdf = Join-Path $work 'chart.pdf'
        $uri = ([Uri]$htmlPath).AbsoluteUri
        $index = 0
        foreach ($browser in $browsers) {
            $index++
            $profileDir = Join-Path $work "profile-$index"
            $arguments = @('--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check', "--user-data-dir=`"$profileDir`"", '--no-pdf-header-footer', "--print-to-pdf=`"$tempPdf`"", $uri)
            $process = Start-Process -FilePath $browser -ArgumentList $arguments -PassThru -WindowStyle Hidden
            if (-not $process.WaitForExit(120000)) {
                try { $process.Kill($true) } catch { }
                continue
            }
            if (Test-Path -LiteralPath $tempPdf -PathType Leaf) {
                $usedBrowser = [IO.Path]::GetFileNameWithoutExtension($browser)
                break
            }
        }
        if (-not (Test-Path -LiteralPath $tempPdf -PathType Leaf)) { Fail 'No browser wrote a PDF.' }

        $text = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($tempPdf))
        $pages = [regex]::Matches($text, '/Type\s*/Page(?![a-zA-Z])').Count
        if ($pages -ne 1) { Fail "The PDF has $pages pages; expected 1." }
        $box = [regex]::Match($text, '/MediaBox\s*\[\s*([-0-9.]+)\s+([-0-9.]+)\s+([-0-9.]+)\s+([-0-9.]+)\s*\]')
        if (-not $box.Success) { Fail 'The PDF page size could not be read.' }
        $widthMm = ([double]$box.Groups[3].Value - [double]$box.Groups[1].Value) / $pointsPerMm
        $heightMm = ([double]$box.Groups[4].Value - [double]$box.Groups[2].Value) / $pointsPerMm
        if ([math]::Abs($widthMm - 841) -gt $toleranceMm -or [math]::Abs($heightMm - 1189) -gt $toleranceMm) {
            Fail ('The PDF page is {0:N2} x {1:N2} mm; expected 841 x 1189 mm within {2} mm.' -f $widthMm, $heightMm, $toleranceMm)
        }
        if ($text -match '/Subtype\s*/Image') { Fail 'The PDF contains an image XObject; the chart must stay vector.' }

        Copy-Item -LiteralPath $tempPdf -Destination $output -Force
        $pdfHash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
        Write-Host ("Chart          : {0} v{1} ({2})" -f $spec.printMaster.chartId, $spec.printMaster.chartVersion, $spec.printMaster.intendedSplit)
        Write-Host ("Master SHA-256 : {0}" -f $masterHash)
        Write-Host ("Browser        : {0}" -f $usedBrowser)
        Write-Host ("PDF page       : {0:N2} x {1:N2} mm, 1 page, no image XObject" -f $widthMm, $heightMm)
        Write-Host ("PDF SHA-256    : {0}" -f $pdfHash)
        Write-Host ("PDF size       : {0} bytes" -f (Get-Item -LiteralPath $output).Length)
        exit 0
    }
    finally {
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}
catch {
    Fail "Pilot chart PDF export failed: $($_.Exception.Message)"
}
