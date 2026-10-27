<#
.SYNOPSIS
  Draws the ring-lattice chart that the fiducial detector reads, as a full-size vector PDF for
  printing, with its chart definition and a detector job template. Issue #371.

.DESCRIPTION
  The chart follows the rules of the fiducial detector (docs/M2_FIDUCIAL_DETECTOR.md) and of the
  synthetic chart it was developed on (src/m2/synthetic_pair_chart.cpp):
    - a ring fiducial (black ring, white ring, black dot) on every node of a square lattice,
      node (column, row) at origin + pitch * (column, row) mm, point id F-<column>-<row>;
    - in every cell the pattern (3 * column + 5 * row) mod 8 (0 and 6 blank, 1 slanted edge dark on
      light, 7 slanted edge light on dark, 2 colour patch, 5 grey patch with the level
      (column + row) mod 6, 3 vertical fine lines, 4 horizontal fine lines);
    - lattice lines on the node rows and columns.
  The sizes are the generator's ratios to the pitch (ring outer radius 0.14 pitch, white ring 0.6 R,
  dot 0.25 R, pattern half size 0.25 pitch, slanted edge backing 0.3 pitch). Grey levels are the
  generator's levels divided by its paper level 0.92, so that the bare paper of the print takes the
  place of the generator's paper (the detector compares levels as ratios to the paper of the same
  cell). See docs/CALIBRATION_CAPTURE_PROCEDURE.md for the reasons behind each value.

  The lattice is centred on the sheet. The margin is the least distance from the sheet edge to the
  outermost node. The margin carries the chart id, the size of the lattice, a 100 mm scale in both
  directions to check the print scale, and the ids of the four corner nodes.

  Outputs, all in OutputDirectory (outside the repository), named <chart id>.*:
    .svg                         the drawing (deterministic: same arguments, same bytes)
    .pdf                         one page of the paper size, vector only (derived; bytes vary per run)
    .chart.json                  the chart definition (the "chart" object of a detector job, and the design values)
    .detector-job.template.json  a detector job (a0.fiducial-detector-job/1) with 9 views per camera
                                 and empty anchors, to be completed after the capture
  Existing files are not overwritten.

.PARAMETER Paper
  A4, A3 or A0. Default A3 (the paper the owner can print; a default that can be changed, not an approved value).

.PARAMETER Orientation
  portrait or landscape. Default portrait (the reference sheet lies portrait in the overlap of the
  two cameras, so its axes match the document axes).

.PARAMETER MarginMm
  Least distance from the sheet edge to the outermost node, mm. Default 20.

.PARAMETER PitchMm
  Lattice pitch, mm. Default 20.

.PARAMETER Version
  Version of the drawing, v1, v2, ... Raise it when anything in the drawing changes.

.PARAMETER SvgOnly
  Write only the SVG and the two JSON files (no browser is started).

.EXAMPLE
  pwsh -NoProfile -File scripts/Export-DetectorChartPdf.ps1 -OutputDirectory <folder outside the repository>
#>
#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('A4', 'A3', 'A0')][string]$Paper = 'A3',
    [ValidateSet('portrait', 'landscape')][string]$Orientation = 'portrait',
    [double]$MarginMm = 20,
    [double]$PitchMm = 20,
    [string]$Version = 'v1',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$SvgOnly,
    [string]$BrowserPath,
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$invariant = [Globalization.CultureInfo]::InvariantCulture
$pointsPerMm = 72 / 25.4
$toleranceMm = 0.5

function Fail([string]$Message) {
    Write-Error $Message -ErrorAction Continue
    exit 1
}

# Fixed-point text for SVG and JSON: at most 4 decimals, no exponent, no negative zero.
function F([double]$Value) {
    if (-not [double]::IsFinite($Value)) { throw 'Non-finite number in the drawing.' }
    $rounded = [math]::Round($Value, 4)
    if ($rounded -eq 0) { return '0' }
    return $rounded.ToString('0.####', $invariant)
}

function Gray([double]$Level) {
    $v = [int][math]::Round([math]::Min(1.0, [math]::Max(0.0, $Level)) * 255)
    return '#{0:x2}{0:x2}{0:x2}' -f $v
}

function Write-NewFile([string]$Path, [string]$Text) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $stream = [IO.FileStream]::new($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
}

# Design of the chart. Ratios and levels come from src/m2/synthetic_pair_chart.cpp and
# src/m2/fiducial_detector.cpp; see the header of this script.
$generatorPaperLevel = 0.92
$design = [ordered]@{
    ringOuterRadiusRatio  = 0.14     # of the pitch (generator rule: at most 0.15)
    ringWhiteRadiusRatio  = 0.6      # of the ring outer radius
    ringDotRadiusRatio    = 0.25     # of the ring outer radius
    patternHalfRatio      = 0.25     # of the pitch
    edgeBackingHalfRatio  = 0.3      # of the pitch
    slantedEdgeSlope      = 0.0875   # tan of the rotation of the slanted edge square
    fineLineBaseRatio     = 0.025    # of the pitch; bands of 1x..4x (generator rule: at most 1/16)
    gridLineWidthMm       = 0.3
}
$levels = [ordered]@{
    ink       = 0.06 / $generatorPaperLevel
    edgeDark  = 0.08 / $generatorPaperLevel
    edgeLight = [math]::Min(1.0, 0.96 / $generatorPaperLevel)
    gridLine  = 0.30 / $generatorPaperLevel
}
$grayLevels = @(0.05, 0.2, 0.4, 0.6, 0.8, 0.95 | ForEach-Object { [math]::Min(1.0, $_ / $generatorPaperLevel) })
$palette = @(
    @(227, 26, 28), @(51, 160, 44), @(31, 120, 180), @(255, 223, 0),
    @(255, 127, 0), @(106, 61, 154), @(0, 174, 239), @(236, 0, 140),
    @(140, 90, 60), @(242, 201, 160), @(107, 142, 35), @(112, 128, 144)
)

try {
    if ($Version -cnotmatch '^v[1-9][0-9]{0,2}$') { Fail 'Version must look like v1, v2, ...' }
    if (-not ([double]::IsFinite($PitchMm) -and $PitchMm -ge 10 -and $PitchMm -le 100)) { Fail 'PitchMm must be between 10 and 100.' }
    if (-not ([double]::IsFinite($MarginMm) -and $MarginMm -ge 15 -and $MarginMm -le 200)) { Fail 'MarginMm must be between 15 and 200.' }
    if ([math]::Round($PitchMm * 10) -ne $PitchMm * 10 -or [math]::Round($MarginMm * 10) -ne $MarginMm * 10) { Fail 'PitchMm and MarginMm take at most one decimal.' }

    $root = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $outDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if ($outDir -eq $root -or $outDir.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        Fail 'OutputDirectory is inside the repository. Write the chart to a folder outside the repository.'
    }
    if (-not (Test-Path -LiteralPath $outDir -PathType Container)) { Fail 'OutputDirectory does not exist.' }

    $sizes = @{ A4 = @(210, 297); A3 = @(297, 420); A0 = @(841, 1189) }
    $short = $sizes[$Paper][0]; $long = $sizes[$Paper][1]
    $width = if ($Orientation -eq 'portrait') { $short } else { $long }
    $height = if ($Orientation -eq 'portrait') { $long } else { $short }

    $pitch = $PitchMm
    $columns = [int][math]::Floor(($width - 2 * $MarginMm) / $pitch + 1e-9)
    $rows = [int][math]::Floor(($height - 2 * $MarginMm) / $pitch + 1e-9)
    if ($columns -lt 4 -or $rows -lt 4) { Fail "The lattice has $columns x $rows cells; at least 4 x 4 are needed. Use a smaller pitch or margin." }
    if ($columns -gt 999 -or $rows -gt 999) { Fail 'The lattice has more than 999 cells in one direction (the point id has three digits).' }
    $originX = ($width - $columns * $pitch) / 2
    $originY = ($height - $rows * $pitch) / 2
    $ringR = $design.ringOuterRadiusRatio * $pitch
    $ringWhite = $design.ringWhiteRadiusRatio * $ringR
    $ringDot = $design.ringDotRadiusRatio * $ringR
    $half = $design.patternHalfRatio * $pitch
    $backing = $design.edgeBackingHalfRatio * $pitch
    $fineBase = $design.fineLineBaseRatio * $pitch
    # The labels and scales need a free band between the sheet edge and the rings.
    $band = [math]::Min($originX, $originY) - $ringR
    if ($band -lt 15) { Fail ('The free band outside the rings is {0} mm; at least 15 mm is needed for the labels and scales. Use a larger margin.' -f (F $band)) }

    $grayCells = 0
    for ($r = 0; $r -lt $rows; $r++) { for ($c = 0; $c -lt $columns; $c++) { if (((3 * $c + 5 * $r) % 8) -eq 5) { $grayCells++ } } }

    $chartId = ('a0cs-detector-chart-{0}-{1}-p{2}-m{3}-{4}' -f $Paper.ToLowerInvariant(), $Orientation, (F $pitch), (F $MarginMm), $Version)

    # ---- SVG -------------------------------------------------------------------------------
    $sb = [Text.StringBuilder]::new()
    $null = $sb.Append('<?xml version="1.0" encoding="UTF-8"?>' + "`n")
    $null = $sb.Append(('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="{0}mm" height="{1}mm" viewBox="0 0 {0} {1}" data-chart-id="{2}">' -f (F $width), (F $height), $chartId) + "`n")
    $null = $sb.Append(('<title>{0}</title>' -f $chartId) + "`n")
    $null = $sb.Append('<defs>' + "`n")
    $null = $sb.Append(('<g id="ring"><circle r="{0}" fill="{3}"/><circle r="{1}" fill="#ffffff"/><circle r="{2}" fill="{3}"/></g>' -f (F $ringR), (F $ringWhite), (F $ringDot), (Gray $levels.ink)) + "`n")
    $angle = [math]::Atan($design.slantedEdgeSlope) * 180 / [math]::PI
    $null = $sb.Append(('<g id="edge-dark-on-light"><rect x="{0}" y="{0}" width="{1}" height="{1}" fill="{4}"/><rect x="{2}" y="{2}" width="{3}" height="{3}" fill="{5}" transform="rotate({6})"/></g>' -f (F (-$backing)), (F (2 * $backing)), (F (-$half)), (F (2 * $half)), (Gray $levels.edgeLight), (Gray $levels.edgeDark), (F $angle)) + "`n")
    $null = $sb.Append(('<g id="edge-light-on-dark"><rect x="{0}" y="{0}" width="{1}" height="{1}" fill="{4}"/><rect x="{2}" y="{2}" width="{3}" height="{3}" fill="{5}" transform="rotate({6})"/></g>' -f (F (-$backing)), (F (2 * $backing)), (F (-$half)), (F (2 * $half)), (Gray $levels.edgeDark), (Gray $levels.edgeLight), (F $angle)) + "`n")
    # Fine lines: four bands along the lines with widths 1x..4x the base width; dark stripes start at
    # the edge of the pattern square (generator: phase = (across + half) / (2 width), dark below 0.5).
    foreach ($kind in @('v', 'h')) {
        $null = $sb.Append(('<g id="fine-lines-{0}" fill="{1}">' -f $kind, (Gray $levels.ink)))
        $bandLength = $half / 2
        for ($b = 0; $b -lt 4; $b++) {
            $lineWidth = $fineBase * ($b + 1)
            $alongStart = -$half + $b * $bandLength
            for ($k = 0; ; $k++) {
                $acrossStart = -$half + 2 * $k * $lineWidth
                if ($acrossStart -ge $half - 1e-9) { break }
                $acrossEnd = [math]::Min($acrossStart + $lineWidth, $half)
                if ($kind -eq 'v') {
                    $null = $sb.Append(('<rect x="{0}" y="{1}" width="{2}" height="{3}"/>' -f (F $acrossStart), (F $alongStart), (F ($acrossEnd - $acrossStart)), (F $bandLength)))
                } else {
                    $null = $sb.Append(('<rect x="{1}" y="{0}" width="{3}" height="{2}"/>' -f (F $acrossStart), (F $alongStart), (F ($acrossEnd - $acrossStart)), (F $bandLength)))
                }
            }
        }
        $null = $sb.Append('</g>' + "`n")
    }
    $null = $sb.Append('</defs>' + "`n")

    # Lattice lines on the node rows and columns.
    $extentX = $columns * $pitch
    $extentY = $rows * $pitch
    $null = $sb.Append(('<g id="lattice-lines" stroke="{0}" stroke-width="{1}" fill="none">' -f (Gray $levels.gridLine), (F $design.gridLineWidthMm)))
    for ($c = 0; $c -le $columns; $c++) {
        $x = $originX + $c * $pitch
        $null = $sb.Append(('<line x1="{0}" y1="{1}" x2="{0}" y2="{2}"/>' -f (F $x), (F $originY), (F ($originY + $extentY))))
    }
    for ($r = 0; $r -le $rows; $r++) {
        $y = $originY + $r * $pitch
        $null = $sb.Append(('<line x1="{0}" y1="{1}" x2="{2}" y2="{1}"/>' -f (F $originX), (F $y), (F ($originX + $extentX))))
    }
    $null = $sb.Append('</g>' + "`n")

    # Cell patterns.
    $null = $sb.Append('<g id="cell-patterns">' + "`n")
    for ($r = 0; $r -lt $rows; $r++) {
        for ($c = 0; $c -lt $columns; $c++) {
            $cx = $originX + ($c + 0.5) * $pitch
            $cy = $originY + ($r + 0.5) * $pitch
            switch ((3 * $c + 5 * $r) % 8) {
                1 { $null = $sb.Append(('<use xlink:href="#edge-dark-on-light" x="{0}" y="{1}"/>' -f (F $cx), (F $cy))) }
                7 { $null = $sb.Append(('<use xlink:href="#edge-light-on-dark" x="{0}" y="{1}"/>' -f (F $cx), (F $cy))) }
                3 { $null = $sb.Append(('<use xlink:href="#fine-lines-v" x="{0}" y="{1}"/>' -f (F $cx), (F $cy))) }
                4 { $null = $sb.Append(('<use xlink:href="#fine-lines-h" x="{0}" y="{1}"/>' -f (F $cx), (F $cy))) }
                2 {
                    $rgb = $palette[($c + 2 * $r) % $palette.Count]
                    $null = $sb.Append(('<rect x="{0}" y="{1}" width="{2}" height="{2}" fill="#{3:x2}{4:x2}{5:x2}"/>' -f (F ($cx - $half)), (F ($cy - $half)), (F (2 * $half)), $rgb[0], $rgb[1], $rgb[2]))
                }
                5 {
                    $null = $sb.Append(('<rect x="{0}" y="{1}" width="{2}" height="{2}" fill="{3}"/>' -f (F ($cx - $half)), (F ($cy - $half)), (F (2 * $half)), (Gray $grayLevels[($c + $r) % 6])))
                }
                default { }
            }
        }
        $null = $sb.Append("`n")
    }
    $null = $sb.Append('</g>' + "`n")

    # Ring fiducials on every node, drawn last.
    $null = $sb.Append('<g id="ring-fiducials">' + "`n")
    for ($r = 0; $r -le $rows; $r++) {
        for ($c = 0; $c -le $columns; $c++) {
            $null = $sb.Append(('<use xlink:href="#ring" x="{0}" y="{1}" data-point-id="F-{2:d3}-{3:d3}"/>' -f (F ($originX + $c * $pitch)), (F ($originY + $r * $pitch)), $c, $r))
        }
        $null = $sb.Append("`n")
    }
    $null = $sb.Append('</g>' + "`n")

    # Margin: chart id, corner node ids, 100 mm scales.
    $text = Gray 0
    $null = $sb.Append(('<g id="labels" font-family="Arial, Helvetica, sans-serif" fill="{0}">' -f $text) + "`n")
    $null = $sb.Append(('<text x="{0}" y="9" font-size="3" text-anchor="middle">{1}  |  {2} x {3} cells, pitch {4} mm  |  print at 100 % (actual size), do not fit to page</text>' -f (F ($width / 2)), $chartId, $columns, $rows, (F $pitch)) + "`n")
    $leftX = $originX - $ringR - 1
    $rightX = $originX + $extentX + $ringR + 1
    foreach ($corner in @(@(0, 0), @($columns, 0), @(0, $rows), @($columns, $rows))) {
        $isLeft = $corner[0] -eq 0
        $x = if ($isLeft) { $leftX } else { $rightX }
        $anchor = if ($isLeft) { 'end' } else { 'start' }
        $y = $originY + $corner[1] * $pitch + 0.8
        $null = $sb.Append(('<text x="{0}" y="{1}" font-size="2.2" text-anchor="{2}">F-{3:d3}-{4:d3}</text>' -f (F $x), (F $y), $anchor, $corner[0], $corner[1]) + "`n")
    }
    $null = $sb.Append('</g>' + "`n")
    $null = $sb.Append(('<g id="scale-100mm" stroke="{0}" stroke-width="0.2" fill="none">' -f $text))
    $hx = $width / 2 - 50; $hy = $height - 8
    $null = $sb.Append(('<line x1="{0}" y1="{1}" x2="{2}" y2="{1}"/>' -f (F $hx), (F $hy), (F ($hx + 100))))
    $vx = 8; $vy = $height / 2 - 50
    $null = $sb.Append(('<line x1="{0}" y1="{1}" x2="{0}" y2="{2}"/>' -f (F $vx), (F $vy), (F ($vy + 100))))
    for ($t = 0; $t -le 10; $t++) {
        $len = if ($t % 5 -eq 0) { 3 } else { 1.5 }
        $null = $sb.Append(('<line x1="{0}" y1="{1}" x2="{0}" y2="{2}"/>' -f (F ($hx + 10 * $t)), (F $hy), (F ($hy - $len))))
        $null = $sb.Append(('<line x1="{0}" y1="{1}" x2="{2}" y2="{1}"/>' -f (F $vx), (F ($vy + 10 * $t)), (F ($vx + $len))))
    }
    $null = $sb.Append('</g>' + "`n")
    $null = $sb.Append(('<g font-family="Arial, Helvetica, sans-serif" fill="{0}" font-size="2.2">' -f $text))
    $null = $sb.Append(('<text x="{0}" y="{1}">100 mm</text>' -f (F ($hx + 102)), (F ($hy + 0.8))))
    $null = $sb.Append(('<text x="{0}" y="{1}" text-anchor="middle" transform="rotate(-90 {0} {1})">100 mm</text>' -f (F ($vx + 5)), (F ($vy + 50))))
    $null = $sb.Append('</g>' + "`n")
    $null = $sb.Append('</svg>' + "`n")
    $svgText = $sb.ToString()
    $svgSha = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.UTF8Encoding]::new($false).GetBytes($svgText))).ToLowerInvariant()

    # ---- chart definition and job template ---------------------------------------------------
    $chartObject = [ordered]@{ columns = $columns; rows = $rows; originXMm = $originX; originYMm = $originY; pitchMm = $pitch }
    $definition = [ordered]@{
        schema     = 'a0.detector-chart/1'
        chartId    = $chartId
        version    = $Version
        paper      = [ordered]@{ name = $Paper; orientation = $Orientation; widthMm = $width; heightMm = $height }
        marginMm   = $MarginMm
        chart      = $chartObject
        nodes      = [ordered]@{ count = ($columns + 1) * ($rows + 1); firstPointId = 'F-000-000'; lastPointId = ('F-{0:d3}-{1:d3}' -f $columns, $rows) }
        design     = [ordered]@{
            ringOuterRadiusMm   = [math]::Round($ringR, 4)
            ringWhiteRadiusMm   = [math]::Round($ringWhite, 4)
            ringDotRadiusMm     = [math]::Round($ringDot, 4)
            patternHalfSizeMm   = [math]::Round($half, 4)
            edgeBackingHalfMm   = [math]::Round($backing, 4)
            slantedEdgeSlope    = $design.slantedEdgeSlope
            fineLineBaseWidthMm = [math]::Round($fineBase, 4)
            gridLineWidthMm     = $design.gridLineWidthMm
            patternRule         = '(3 * column + 5 * row) mod 8; 0 and 6 blank, 1 slanted edge dark on light, 7 slanted edge light on dark, 2 colour, 5 grey, 3 vertical fine lines, 4 horizontal fine lines'
            grayRule            = '(column + row) mod 6 over the levels 0.05, 0.2, 0.4, 0.6, 0.8, 0.95 of the generator paper 0.92, printed as level / 0.92 of the bare paper (at most 1)'
            grayCells           = $grayCells
        }
        scaleBarsMm = 100
        svgSha256   = $svgSha
    }
    $views = [Collections.Generic.List[object]]::new()
    foreach ($camera in @('CAM-A', 'CAM-B')) {
        $roles = [Collections.Generic.List[string[]]]::new()
        for ($i = 1; $i -le 6; $i++) { $roles.Add(@('training', ('training-{0:d2}' -f $i))) }
        for ($i = 1; $i -le 2; $i++) { $roles.Add(@('validation', ('validation-{0:d2}' -f $i))) }
        $roles.Add(@('reference', 'reference'))
        foreach ($entry in $roles) {
            $views.Add([ordered]@{ viewId = "$camera-$($entry[1])"; camera = $camera; role = $entry[0]; image = "$camera/$($entry[1]).jpg"; anchors = @() })
        }
    }
    $cameraTemplate = [ordered]@{ widthPixels = 7360; heightPixels = 4912; lensLabel = 'TO-BE-FILLED'; focalLengthLabel = 'TO-BE-FILLED'; focusLabel = 'TO-BE-FILLED' }
    $job = [ordered]@{
        schema        = 'a0.fiducial-detector-job/1'
        sessionId     = 'TO-BE-FILLED'
        chartId       = $chartId
        measuredAtUtc = 'YYYY-MM-DDThh:mm:ssZ'
        chart         = $chartObject
        cameras       = [ordered]@{ 'CAM-A' = $cameraTemplate; 'CAM-B' = $cameraTemplate }
        views         = $views
    }

    $base = Join-Path $outDir $chartId
    $svgPath = "$base.svg"; $pdfPath = "$base.pdf"; $chartPath = "$base.chart.json"; $jobPath = "$base.detector-job.template.json"
    $targets = @($svgPath, $chartPath, $jobPath) + $(if ($SvgOnly) { @() } else { @($pdfPath) })
    foreach ($target in $targets) { if (Test-Path -LiteralPath $target) { Fail "$([IO.Path]::GetFileName($target)) already exists in OutputDirectory; it is not overwritten." } }

    # ---- PDF ---------------------------------------------------------------------------------
    $pdfLine = 'not written (-SvgOnly)'
    $pdfBytes = $null
    if (-not $SvgOnly) {
        # Chrome first: on some PCs Edge exits without writing the PDF (Export-PilotChartPdf.ps1).
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
        $work = Join-Path ([IO.Path]::GetTempPath()) ('a0-detector-chart-pdf-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $work | Out-Null
        try {
            # The SVG is inlined; an SVG loaded through <img> is rasterized when printed.
            $inline = $svgText.Substring($svgText.IndexOf('<svg', [StringComparison]::Ordinal))
            $html = "<!doctype html>`n<html><head><meta charset=`"utf-8`"><style>`n@page { size: $(F $width)mm $(F $height)mm; margin: 0; }`nhtml, body { margin: 0; padding: 0; }`nsvg { display: block; width: $(F $width)mm; height: $(F $height)mm; }`n</style></head>`n<body>`n$inline</body></html>`n"
            $htmlPath = Join-Path $work 'print.html'
            Set-Content -LiteralPath $htmlPath -Value $html -Encoding utf8NoBOM
            $tempPdf = Join-Path $work 'chart.pdf'
            $uri = ([Uri]$htmlPath).AbsoluteUri
            $usedBrowser = $null
            $index = 0
            foreach ($browser in $browsers) {
                $index++
                $profileDir = Join-Path $work "profile-$index"
                $arguments = @('--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check', "--user-data-dir=`"$profileDir`"", '--no-pdf-header-footer', "--print-to-pdf=`"$tempPdf`"", $uri)
                $process = Start-Process -FilePath $browser -ArgumentList $arguments -PassThru -WindowStyle Hidden
                if (-not $process.WaitForExit(180000)) { try { $process.Kill($true) } catch { }; continue }
                if (Test-Path -LiteralPath $tempPdf -PathType Leaf) { $usedBrowser = [IO.Path]::GetFileNameWithoutExtension($browser); break }
            }
            if (-not $usedBrowser) { Fail 'No browser wrote a PDF.' }

            $pdfBytes = [IO.File]::ReadAllBytes($tempPdf)
            $pdfText = [Text.Encoding]::Latin1.GetString($pdfBytes)
            $pages = [regex]::Matches($pdfText, '/Type\s*/Page(?![a-zA-Z])').Count
            if ($pages -ne 1) { Fail "The PDF has $pages pages; expected 1." }
            $box = [regex]::Match($pdfText, '/MediaBox\s*\[\s*([-0-9.]+)\s+([-0-9.]+)\s+([-0-9.]+)\s+([-0-9.]+)\s*\]')
            if (-not $box.Success) { Fail 'The PDF page size could not be read.' }
            $pageW = ([double]::Parse($box.Groups[3].Value, $invariant) - [double]::Parse($box.Groups[1].Value, $invariant)) / $pointsPerMm
            $pageH = ([double]::Parse($box.Groups[4].Value, $invariant) - [double]::Parse($box.Groups[2].Value, $invariant)) / $pointsPerMm
            if ([math]::Abs($pageW - $width) -gt $toleranceMm -or [math]::Abs($pageH - $height) -gt $toleranceMm) {
                Fail ('The PDF page is {0:N2} x {1:N2} mm; expected {2} x {3} mm within {4} mm.' -f $pageW, $pageH, $width, $height, $toleranceMm)
            }
            if ($pdfText -match '/Subtype\s*/Image') { Fail 'The PDF contains an image XObject; the chart must stay vector.' }
            $pdfSha = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($pdfBytes)).ToLowerInvariant()
            $pdfLine = ('{0:N2} x {1:N2} mm, 1 page, no image XObject, {2} bytes, browser {3}, SHA-256 {4}' -f $pageW, $pageH, $pdfBytes.Length, $usedBrowser, $pdfSha)
        }
        finally {
            Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Write-NewFile $svgPath $svgText
    Write-NewFile $chartPath (($definition | ConvertTo-Json -Depth 8) + "`n")
    Write-NewFile $jobPath (($job | ConvertTo-Json -Depth 8) + "`n")
    if ($pdfBytes) {
        $stream = [IO.FileStream]::new($pdfPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $stream.Write($pdfBytes, 0, $pdfBytes.Length) } finally { $stream.Dispose() }
    }

    Write-Host ('Chart          : {0}' -f $chartId)
    Write-Host ('Paper          : {0} {1}, {2} x {3} mm, margin {4} mm' -f $Paper, $Orientation, $width, $height, (F $MarginMm))
    Write-Host ('Lattice        : {0} x {1} cells, {2} nodes, pitch {3} mm, node F-000-000 at ({4}, {5}) mm, {6} grey cells' -f $columns, $rows, (($columns + 1) * ($rows + 1)), (F $pitch), (F $originX), (F $originY), $grayCells)
    Write-Host ('Ring           : outer radius {0} mm, white ring {1} mm, dot {2} mm' -f (F $ringR), (F $ringWhite), (F $ringDot))
    Write-Host ('SVG SHA-256    : {0}' -f $svgSha)
    Write-Host ('PDF            : {0}' -f $pdfLine)
    Write-Host ('Files          : {0}' -f (($targets | ForEach-Object { [IO.Path]::GetFileName($_) }) -join ', '))
    exit 0
}
catch {
    Fail "Detector chart export failed: $($_.Exception.Message)"
}
