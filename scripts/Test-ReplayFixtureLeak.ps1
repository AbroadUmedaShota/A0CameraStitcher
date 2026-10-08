<#
.SYNOPSIS
  Local check before pushing: compares what the push would publish against the real identifiers
  on this PC. Issues #241 and #243.

.DESCRIPTION
  The replay fixtures (tests/fixtures/hardware-replay) come from real operator sessions and the
  repository is public. The shape checks in the C# tests (HardwareReplayAnonymizationRules.Scan)
  cannot find a value whose shape is unremarkable, such as a 7-character body serial or an
  approval GUID. This script closes that gap.

  Real values are read at run time and kept in memory only:
    - the records under %LOCALAPPDATA%\A0CameraStitcher (IDs, hashes, run IDs, sizes, dates, paths)
    - EXIF of the original photographs found there and in the export folders named in the records
      (owner, serials, dates, unique ID, maker note), read only
    - the registry (Nikon USB / WPD device keys, registered owner)
    - the environment (PC name, user profile paths)
  Nothing is written to disk and no value or hash of one is stored in the repository. Output has
  only labels (category#number) and locations (commit, file, line). Values are never printed;
  neither are absolute paths or the messages of exceptions.

  What is compared (the content a push publishes): for every commit in <Base>..<Head>, the commit
  message, the author and committer name and e-mail, every path the commit touches (a rename
  counts both its old and its new path), the added lines of the diff, the decoded text of hex and
  base64 strings found in added lines (including terminalResultHex), and the decoded content of
  every added or changed .b64 file. Each needle is also tried as dashed GUID, as hex of its UTF-8
  and of its UTF-16LE bytes, and as base64 in the three alignments. The added lines are also read
  in normalized forms: hex written with separators or as 0x / \x arrays (decoded as UTF-8, Latin-1
  and UTF-16LE), JSON, URL and HTML escapes restored, and the lines joined without line breaks and
  indentation (so a value wrapped over two lines, or a base64 string wrapped at 76 characters, is
  still found). A decoded base64 or hex string that starts like an image (JPEG, PNG, GIF, TIFF,
  WebP) is reported as a hit; a .b64 file is accepted as an image only when it is a small dummy
  carrying the synthetic-image notice. Binary files in the range are reported as hits.
  -IncludeChangedFiles also compares the whole working tree content of every file the range
  touches.

  Exit codes: 0 = no hits, 1 = hits (judge each: a hit may be a false positive such as a real date
  that also appears in prose, but a real value must not be pushed), 2 = could not verify
  (no local source data, no body serial number to compare, a photograph folder that was too large
  to read completely, or an error). Treat 2 as "do not push".

  The check refuses to call a push clean when it has no body serial number to look for (neither
  from the EXIF of a photograph nor from the Nikon USB / WPD registry entries of this PC): a PC
  without the original photographs and without the camera's registry entries cannot verify.

.PARAMETER Base
  The revision the push starts from. Default: the upstream of the current branch, otherwise
  origin/main.

.PARAMETER Head
  The last revision to publish. Default: HEAD.

.PARAMETER ExtraNeedle
  Extra values to look for (for example an approval reference you know by heart). They stay in
  memory, but they are on this command line, so prefer the automatic sources.

.PARAMETER ExtraPhotoRoot
  Extra folders searched for original photographs (*.jpg, *.jpeg) whose EXIF is read.

.PARAMETER NoLocalSources
  Only valid together with -SelfTest. In a normal run it is refused (exit 2): skipping the real
  values is not a way to get a clean result.

.PARAMETER SelfTest
  Runs the check against a throw-away repository with invented values and reports PASS or FAIL.
  Needs no local source data.

.EXAMPLE
  pwsh -NoProfile -File scripts/Test-ReplayFixtureLeak.ps1 -Base origin/main
#>
#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$Base,
    [string]$Head = 'HEAD',
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$SourceRoot = (Join-Path $env:LOCALAPPDATA 'A0CameraStitcher'),
    [string[]]$ExtraNeedle = @(),
    [string[]]$ExtraPhotoRoot = @(),
    [switch]$IncludeChangedFiles,
    [switch]$NoLocalSources,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$script:Utf8 = [Text.UTF8Encoding]::new($false)
$script:Latin1 = [Text.Encoding]::Latin1
$script:Utf16 = [Text.Encoding]::Unicode
# More photographs than this under one folder cannot be read completely: report it (exit 2).
$script:MaxPhotosPerRoot = 2000
$script:SyntheticImageNotice = 'A0 replay fixture: synthetic image, not a photograph'
$script:SyntheticImageMaxBytes = 4096
# A decoded string shorter than this is a header, not a picture (the array of a JPEG header in code, say).
# It stays below one line of wrapped base64 (48 bytes at 64 columns, 57 at 76) so that each line of a
# wrapped picture is still checked on its own when joining the lines shifts the start of the picture.
$script:MinEmbeddedImageBytes = 32
# Hashes of the fixture dummy images at Head (set for each range; see Get-HeadDummyImageHashes).
$script:AllowedImageSha256 = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)

# Plain English words that are also short account or owner names. A short plain-word needle is
# skipped only when it equals the account name (the profile path forms cover that) or is listed
# here; any other short word is compared as a whole word, case-sensitively.
$script:GenericWords = @(
    'user', 'users', 'admin', 'administrator', 'owner', 'test', 'tester', 'demo', 'guest', 'home',
    'host', 'name', 'none', 'null', 'temp', 'work', 'info', 'data', 'main', 'root', 'this', 'that',
    'nikon', 'camera', 'photo', 'photos', 'local', 'public', 'default', 'system', 'unknown', 'n/a')

# A thrown error whose message is fixed text only (no path, no value, no tool output).
function New-SafeError {
    param([string]$Message)
    $e = [System.InvalidOperationException]::new($Message)
    $e.Data['SafeMessage'] = $Message
    return $e
}

# ------------------------------------------------------------------ needles

function New-NeedleSet {
    [pscustomobject]@{
        List  = [System.Collections.Generic.List[object]]::new()
        Seen  = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        Count = @{}
        Skipped = @{}
    }
}

function Add-Needle {
    param($Set, [string]$Value, [string]$Category, [string]$AccountName = $env:USERNAME)
    if ([string]::IsNullOrWhiteSpace($Value)) { return }
    $Value = $Value.Trim()
    if ($Value.Length -lt 4) { return }
    $strict = $false
    # A short plain word (an account or owner name such as a bare first name) matches ordinary
    # prose. It is skipped only when it is the account name (the profile path forms below cover
    # the real exposure) or a generic word; otherwise it is kept and compared as a whole word,
    # case-sensitively.
    if ($Category -in 'regowner', 'username', 'pcname' -and $Value.Length -lt 5 -and $Value -match '^[A-Za-z]+$') {
        $reason = $null
        if (-not [string]::IsNullOrWhiteSpace($AccountName) -and $Value -ieq $AccountName) { $reason = 'account name' }
        elseif ($script:GenericWords -contains $Value.ToLowerInvariant()) { $reason = 'generic word' }
        if ($reason) {
            $key = "$Category ($reason)"
            if (-not $Set.Skipped.ContainsKey($key)) { $Set.Skipped[$key] = 0 }
            $Set.Skipped[$key]++
            return
        }
        $strict = $true
    }
    if (-not $Set.Seen.Add($Value)) { return }
    if (-not $Set.Count.ContainsKey($Category)) { $Set.Count[$Category] = 0 }
    $Set.Count[$Category]++
    $Set.List.Add([pscustomobject]@{ Value = $Value; Category = $Category; Label = "$Category#$($Set.Count[$Category])"; Strict = $strict })
}

# A serial number as read, and without its leading zeros (USB / WPD identifiers pad the serial
# that EXIF and the camera menu show without padding).
function Add-SerialNeedle {
    param($Set, [string]$Value, [string]$Category)
    if ([string]::IsNullOrWhiteSpace($Value)) { return }
    $Value = $Value.Trim()
    Add-Needle $Set $Value $Category
    $trimmed = $Value.TrimStart('0')
    if ($trimmed -ne $Value) { Add-Needle $Set $trimmed $Category }
}

# Registry key names of Nikon USB / WPD devices: the serial sits after VID_04B0&PID_xxxx.
function Add-DeviceNameNeedles {
    param($Set, [string]$Name)
    if ($Name -match '(?i)VID_04B0&PID_[0-9A-F]{4}[#\\]([^#\\]+)') {
        Add-SerialNeedle $Set $Matches[1] 'usb-serial'
        Add-Needle $Set $Name 'wpd-devicekey'
        return $true
    }
    return $false
}

$script:TextExtensions = @('.json', '.jsonl', '.txt', '.log', '.md', '.csv', '.yaml', '.yml', '.xml', '.ini', '')

function Add-NeedlesFromText {
    param($Set, [string]$Text, $ExportDirs)
    foreach ($m in [regex]::Matches($Text, '(?i)(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])')) { Add-Needle $Set $m.Value.ToLowerInvariant() 'hex64' }
    foreach ($m in [regex]::Matches($Text, '(?i)(?<![0-9a-f])[0-9a-f]{32}(?![0-9a-f])')) { Add-Needle $Set $m.Value.ToLowerInvariant() 'hex32' }
    foreach ($m in [regex]::Matches($Text, '(?:dual-leg-CAM-[AB]-)?run-\d{13}-\d+|hybrid-tx-\d{13}-\d+')) { Add-Needle $Set $m.Value 'runid' }
    foreach ($m in [regex]::Matches($Text, '(?<!\d)\d{13}(?!\d)')) { Add-Needle $Set $m.Value 'epoch13' }
    foreach ($m in [regex]::Matches($Text, '(?<!\d)1[5-9]\d{8}(?!\d)')) { Add-Needle $Set $m.Value 'epoch10' }
    foreach ($m in [regex]::Matches($Text, 'app:[0-9a-fA-F]{32}')) { Add-Needle $Set $m.Value 'approvalref' }
    foreach ($m in [regex]::Matches($Text, '[A-Za-z0-9]+(?:-[A-Za-z0-9]+)*-\d{8}')) { Add-Needle $Set $m.Value 'profileid' }
    foreach ($m in [regex]::Matches($Text, '"(?:bytes|originalSizeBytes|sizeBytes)"\s*:\s*(\d{5,})')) { Add-Needle $Set $m.Groups[1].Value 'origsize' }
    foreach ($m in [regex]::Matches($Text, '(?<!\d)20\d{2}(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\d|3[01])(?!\d)')) { Add-Needle $Set $m.Value 'date8' }
    foreach ($m in [regex]::Matches($Text, '20\d{2}-\d{2}-\d{2}')) { Add-Needle $Set $m.Value 'dateiso' }
    foreach ($m in [regex]::Matches($Text, '(?<!\d)20\d{2}[/:.](?:0[1-9]|1[0-2])[/:.](?:0[1-9]|[12]\d|3[01])(?!\d)')) { Add-Needle $Set $m.Value 'datesep' }
    foreach ($m in [regex]::Matches($Text, '"exportDirectory"\s*:\s*"([^"]+)"')) {
        $p = $m.Groups[1].Value.Replace('\\', '\')
        Add-Needle $Set (Split-Path $p -Leaf) 'exportdir'
        Add-Needle $Set $p 'exportpath'
        # The folder the operator exported to holds copies of the photographs: read them too.
        if ($null -ne $ExportDirs) { $ExportDirs.Add($p) }
    }
    # Absolute paths of the operator's drives (user names appear inside them).
    foreach ($m in [regex]::Matches($Text, '(?i)[A-Z]:\\\\?Users\\\\?[^\\"\r\n]+')) { Add-Needle $Set $m.Value 'userpath' }
}

# --- EXIF reader (read only, first 200 KB of each photograph) ---
function Get-U16([byte[]]$b, [int]$o, [bool]$be) { if ($be) { return ([int]$b[$o] -shl 8) -bor [int]$b[$o + 1] } else { return [int]$b[$o] -bor ([int]$b[$o + 1] -shl 8) } }
function Get-U32([byte[]]$b, [int]$o, [bool]$be) { if ($be) { return ([long]$b[$o] -shl 24) -bor ([long]$b[$o + 1] -shl 16) -bor ([long]$b[$o + 2] -shl 8) -bor [long]$b[$o + 3] } else { return [long]$b[$o] -bor ([long]$b[$o + 1] -shl 8) -bor ([long]$b[$o + 2] -shl 16) -bor ([long]$b[$o + 3] -shl 24) } }
$script:ExifTypeSize = @{ 1 = 1; 2 = 1; 3 = 2; 4 = 4; 5 = 8; 6 = 1; 7 = 1; 8 = 2; 9 = 4; 10 = 8; 11 = 4; 12 = 8; 13 = 4 }

function Get-Ifd([byte[]]$b, [long]$tiffBase, [long]$ifdOffset, [bool]$be) {
    $list = [System.Collections.Generic.List[object]]::new()
    $p = [int]($tiffBase + $ifdOffset)
    if ($p -lt 0 -or $p + 2 -gt $b.Length) { return , $list }
    $n = Get-U16 $b $p $be
    for ($k = 0; $k -lt $n; $k++) {
        $e = $p + 2 + 12 * $k
        if ($e + 12 -gt $b.Length) { break }
        $tag = Get-U16 $b $e $be; $type = Get-U16 $b ($e + 2) $be; $cnt = Get-U32 $b ($e + 4) $be
        $sz = $script:ExifTypeSize[[int]$type]; if (-not $sz) { continue }
        $total = [long]$sz * $cnt
        $off = if ($total -le 4) { [long]$e + 8 } else { $tiffBase + (Get-U32 $b ($e + 8) $be) }
        if ($off + $total -gt $b.Length) { continue }
        $list.Add([pscustomobject]@{ Tag = $tag; Type = $type; Off = [int]$off; Total = [int]$total })
    }
    return , $list
}

function Get-ExifAscii([byte[]]$b, $e) { return $script:Latin1.GetString($b, $e.Off, $e.Total).Split([char]0)[0].Trim() }

function Add-ExifNeedles {
    param($Set, [string]$Path)
    $fs = [IO.File]::OpenRead($Path)
    try { $buf = [byte[]]::new(200000); $null = $fs.Read($buf, 0, $buf.Length) } finally { $fs.Dispose() }
    $i = 2; $app1 = -1
    while ($i + 4 -lt $buf.Length) {
        if ($buf[$i] -ne 0xFF) { break }
        $m = $buf[$i + 1]; $len = ([int]$buf[$i + 2] -shl 8) -bor $buf[$i + 3]
        if ($m -eq 0xE1 -and $app1 -lt 0 -and $script:Latin1.GetString($buf, $i + 4, 4) -eq 'Exif') { $app1 = $i }
        if ($m -eq 0xDA) { break }
        $i += 2 + $len
    }
    if ($app1 -lt 0) { return }
    $tb = $app1 + 10; $be = ($script:Latin1.GetString($buf, $tb, 2) -eq 'MM')
    $e0 = Get-Ifd $buf $tb (Get-U32 $buf ($tb + 4) $be) $be
    $exifIfd = $null
    foreach ($e in $e0) {
        switch ($e.Tag) {
            0x010E { Add-Needle $Set (Get-ExifAscii $buf $e) 'exif-owner' }
            0x013B { Add-Needle $Set (Get-ExifAscii $buf $e) 'exif-owner' }
            0x8298 { Add-Needle $Set (Get-ExifAscii $buf $e) 'exif-owner' }
            0x0132 { $v = Get-ExifAscii $buf $e; Add-Needle $Set $v 'exif-datetime'; if ($v.Length -ge 10) { Add-Needle $Set $v.Substring(0, 10) 'exif-date'; Add-Needle $Set $v.Substring(0, 10).Replace(':', '-') 'exif-date' } }
            0x8769 { $exifIfd = Get-U32 $buf $e.Off $be }
        }
    }
    if ($null -eq $exifIfd) { return }
    foreach ($e in (Get-Ifd $buf $tb $exifIfd $be)) {
        switch ($e.Tag) {
            0x9003 { Add-Needle $Set (Get-ExifAscii $buf $e) 'exif-datetime' }
            0x9004 { Add-Needle $Set (Get-ExifAscii $buf $e) 'exif-datetime' }
            0xA420 { Add-Needle $Set (Get-ExifAscii $buf $e) 'exif-id' }
            0xA430 { Add-Needle $Set (Get-ExifAscii $buf $e) 'exif-owner' }
            0xA431 { Add-SerialNeedle $Set (Get-ExifAscii $buf $e) 'exif-serial' }
            0xA435 { Add-SerialNeedle $Set (Get-ExifAscii $buf $e) 'exif-serial' }
            0x9286 {
                if ($e.Total -gt 8) { Add-Needle $Set ($script:Latin1.GetString($buf, $e.Off + 8, $e.Total - 8).Replace([string][char]0, '').Trim()) 'exif-owner' }
            }
            0x927C {
                if ($script:Latin1.GetString($buf, $e.Off, 5) -eq 'Nikon') {
                    $mb = $e.Off + 10; $mbe = ($script:Latin1.GetString($buf, $mb, 2) -eq 'MM')
                    foreach ($me in (Get-Ifd $buf $mb (Get-U32 $buf ($mb + 4) $mbe) $mbe)) {
                        if ($me.Tag -eq 0x001D -and $me.Type -eq 2) {
                            Add-SerialNeedle $Set (Get-ExifAscii $buf $me) 'exif-serial'
                        }
                        elseif ($me.Tag -eq 0x00A7 -and $me.Type -eq 4) { Add-Needle $Set ([string](Get-U32 $buf $me.Off $mbe)) 'exif-shuttercount' }
                        elseif ($me.Type -eq 2) { $v = Get-ExifAscii $buf $me; if ($v -match '\d{4,}') { Add-Needle $Set $v 'exif-mn-numeric-ascii' } }
                    }
                }
            }
        }
    }
}

# Photographs (*.jpg, *.jpeg) under a folder, read only. Returns the files and whether the folder
# held more than the limit (then not everything was read).
function Get-PhotoFiles {
    param([string]$Root)
    $files = @()
    $capped = $false
    if (Test-Path -LiteralPath $Root -PathType Container) {
        $all = @(Get-ChildItem -LiteralPath $Root -Recurse -File | Where-Object { $_.Extension -in '.jpg', '.jpeg' })
        if ($all.Count -gt $script:MaxPhotosPerRoot) { $capped = $true; $all = @($all | Select-Object -First $script:MaxPhotosPerRoot) }
        $files = $all
    }
    return [pscustomobject]@{ Files = $files; Capped = $capped }
}

function Get-LocalNeedles {
    # NoRegistry and NoEnvironment exist for the self test only: it must not depend on this PC.
    param([string]$SourceRoot, [string[]]$ExtraPhotoRoot, [bool]$NoRegistry = $false, [bool]$NoEnvironment = $false)
    $set = New-NeedleSet
    $sourceFiles = @()
    if (Test-Path -LiteralPath $SourceRoot -PathType Container) {
        $sourceFiles = @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)
    }

    # Records: relative paths and text content (read only).
    $texts = [System.Collections.Generic.List[string]]::new()
    foreach ($f in $sourceFiles) {
        $texts.Add($f.FullName.Substring($SourceRoot.Length))
        if ($f.Length -lt 1MB -and $script:TextExtensions -contains $f.Extension.ToLowerInvariant()) {
            $texts.Add([IO.File]::ReadAllText($f.FullName, $script:Utf8))
        }
    }
    foreach ($t in @($texts)) {
        foreach ($m in [regex]::Matches($t, '"terminalResultHex"\s*:\s*"([0-9a-fA-F]+)"')) {
            if ($m.Groups[1].Value.Length % 2 -eq 0) { $texts.Add($script:Utf8.GetString([Convert]::FromHexString($m.Groups[1].Value))) }
        }
    }
    $exportDirs = [System.Collections.Generic.List[string]]::new()
    foreach ($t in $texts) { Add-NeedlesFromText $set $t $exportDirs }
    # How many camera bodies the records point to: the distinct aliases (CAM-A, CAM-B) they name.
    $aliases = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($t in $texts) {
        foreach ($m in [regex]::Matches($t, '(?<![A-Za-z0-9])CAM-([A-Z])(?![A-Za-z0-9])')) { $null = $aliases.Add($m.Groups[1].Value) }
    }

    # Original photographs: hash, size, EXIF. The folders are the app's own data, the extra roots
    # given on the command line, and the export folders named in the records.
    $capped = $false
    $photoFiles = [System.Collections.Generic.List[object]]::new()
    $seenPhoto = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $own = Get-PhotoFiles $SourceRoot
    if ($own.Capped) { $capped = $true }
    $exportPhotoCount = 0
    $exportDirCount = 0
    foreach ($f in $own.Files) { if ($seenPhoto.Add($f.FullName)) { $photoFiles.Add($f) } }
    foreach ($root in @($ExtraPhotoRoot) + @($exportDirs | Select-Object -Unique)) {
        $isExport = $exportDirs.Contains($root)
        $r = Get-PhotoFiles $root
        if ($r.Capped) { $capped = $true }
        if ($isExport -and (Test-Path -LiteralPath $root -PathType Container)) { $exportDirCount++ }
        foreach ($f in $r.Files) {
            if ($seenPhoto.Add($f.FullName)) { $photoFiles.Add($f); if ($isExport) { $exportPhotoCount++ } }
        }
    }
    foreach ($f in $photoFiles) {
        Add-Needle $set ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([IO.File]::ReadAllBytes($f.FullName))).ToLowerInvariant()) 'origsha-actual'
        Add-Needle $set ([string]$f.Length) 'origsize'
        Add-ExifNeedles $set $f.FullName
    }
    foreach ($n in @($set.List)) {
        if ($n.Category -in 'hex32', 'hex64', 'origsha-actual') { Add-Needle $set $n.Value.Substring(0, 8) ('prefix8-' + $n.Category) }
    }

    $usbCount = 0; $wpdCount = 0
    if (-not $NoEnvironment) {
        # Environment and account.
        Add-Needle $set $env:COMPUTERNAME 'pcname'
        Add-Needle $set $env:USERDOMAIN 'pcname'
        foreach ($v in @($env:USERPROFILE, $env:LOCALAPPDATA, $env:APPDATA, $env:HOMEPATH)) {
            if ([string]::IsNullOrWhiteSpace($v)) { continue }
            Add-Needle $set $v 'userpath'; Add-Needle $set ($v -replace '\\', '/') 'userpath'; Add-Needle $set ($v -replace '\\', '\\') 'userpath'
        }
        if (-not [string]::IsNullOrWhiteSpace($env:USERNAME)) {
            foreach ($v in @("Users\$($env:USERNAME)\", "Users/$($env:USERNAME)/", "Users\\$($env:USERNAME)\\")) { Add-Needle $set $v 'userpath' }
            if ($env:USERNAME.Length -ge 6) { Add-Needle $set $env:USERNAME 'username' }
        }
        $cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue
        if ($cv) { Add-Needle $set $cv.RegisteredOwner 'regowner'; Add-Needle $set $cv.RegisteredOrganization 'regowner' }
    }

    if (-not $NoRegistry) {
        # USB / WPD device identity from the registry (read only). The serial is also tried
        # without its leading zeros.
        foreach ($k in Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB' -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -match '^VID_04B0' }) {
            Add-Needle $set $k.PSChildName 'nikon-vidpid'
            foreach ($c in Get-ChildItem $k.PSPath -ErrorAction SilentlyContinue) {
                $usbCount++
                Add-SerialNeedle $set $c.PSChildName 'usb-serial'
                Add-Needle $set ($k.PSChildName + '\' + $c.PSChildName) 'usb-instanceid'
                Add-Needle $set ($k.PSChildName + '#' + $c.PSChildName) 'usb-instanceid'
            }
        }
        foreach ($root in @('HKLM:\SOFTWARE\Microsoft\Windows Portable Devices\Devices', 'HKLM:\SYSTEM\CurrentControlSet\Enum\SWD\WPDBUSENUM')) {
            foreach ($c in Get-ChildItem $root -ErrorAction SilentlyContinue) {
                if (Add-DeviceNameNeedles $set $c.PSChildName) { $wpdCount++ }
            }
        }
    }
    [pscustomobject]@{
        Set = $set; SourceFiles = $sourceFiles.Count; Photos = $photoFiles.Count; UsbInstances = $usbCount; WpdKeys = $wpdCount
        ExportDirs = $exportDirCount; ExportPhotos = $exportPhotoCount; Capped = $capped; RecordBodies = $aliases.Count
    }
}

# Needles that identify the camera body: the serial from EXIF and from the USB / WPD identifiers.
function Get-BodySerialCount {
    param($Set)
    return @($Set.List | Where-Object { $_.Category -in 'exif-serial', 'usb-serial' }).Count
}

# The same serials counted per body: the padded and the unpadded form (and the EXIF and the USB
# form) of one serial are one body.
function Get-DistinctBodyCount {
    param($Set)
    $bodies = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($n in @($Set.List | Where-Object { $_.Category -in 'exif-serial', 'usb-serial' })) {
        $v = $n.Value.Trim().TrimStart('0')
        if ($v.Length -gt 0) { $null = $bodies.Add($v) }
    }
    return $bodies.Count
}

# Identifiers that come from the records themselves: 32 / 64 digit hex, run IDs, approval references.
# A source folder that yields none of them cannot be the records this check is meant to compare.
function Get-RecordIdCount {
    param($Set)
    $total = 0
    foreach ($c in 'hex32', 'hex64', 'runid', 'approvalref') { if ($Set.Count.ContainsKey($c)) { $total += $Set.Count[$c] } }
    return $total
}

# A line to print when the records are not read from the default folder (never the path itself).
function Get-SourceRootNote {
    param([string]$SourceRoot)
    if ([string]::IsNullOrWhiteSpace($SourceRoot) -or [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) { return $null }
    $default = Join-Path $env:LOCALAPPDATA 'A0CameraStitcher'
    $normalize = { param($p) try { [IO.Path]::GetFullPath($p).TrimEnd('\', '/') } catch { $p } }
    if ((& $normalize $SourceRoot) -ieq (& $normalize $default)) { return $null }
    return 'note: -SourceRoot is not the default source folder; the records of another folder are compared.'
}

# ------------------------------------------------------------------ variants

function New-Variants {
    param($Set)
    $variants = [System.Collections.Generic.List[object]]::new()
    foreach ($n in $Set.List) {
        $v = $n.Value
        $numeric = ($v -match '^\d+$')
        # Boundary: 'digit' for a number (not inside a longer number), 'word' for a short plain
        # word (a whole word, case-sensitive), otherwise none.
        $boundary = if ($n.Strict) { 'word' } elseif ($numeric) { 'digit' } else { 'none' }
        $compare = if ($n.Strict) { [StringComparison]::Ordinal } else { [StringComparison]::OrdinalIgnoreCase }
        $variants.Add([pscustomobject]@{ Needle = $n; Text = $v; Kind = 'plain'; Compare = $compare; Boundary = $boundary })
        if ($n.Category -eq 'hex32') {
            $d = $v.Substring(0, 8) + '-' + $v.Substring(8, 4) + '-' + $v.Substring(12, 4) + '-' + $v.Substring(16, 4) + '-' + $v.Substring(20)
            $variants.Add([pscustomobject]@{ Needle = $n; Text = $d; Kind = 'guid-dashed'; Compare = [StringComparison]::OrdinalIgnoreCase; Boundary = 'none' })
        }
        if ($v.Length -ge 6) {
            $bytes = $script:Utf8.GetBytes($v)
            $variants.Add([pscustomobject]@{ Needle = $n; Text = [Convert]::ToHexString($bytes); Kind = 'hex-encoded'; Compare = [StringComparison]::OrdinalIgnoreCase; Boundary = 'none' })
            $variants.Add([pscustomobject]@{ Needle = $n; Text = [Convert]::ToHexString($script:Utf16.GetBytes($v)); Kind = 'hex-utf16le'; Compare = [StringComparison]::OrdinalIgnoreCase; Boundary = 'none' })
            for ($k = 0; $k -lt 3; $k++) {
                $all = [byte[]](@([byte[]]::new($k) | ForEach-Object { [byte]0x41 }) + $bytes)
                $enc = [Convert]::ToBase64String($all)
                $start = if ($k -eq 0) { 0 } else { 4 }
                $full = [int]([math]::Floor(($k + $bytes.Length) / 3) * 4)
                if ($full - $start -ge 8) {
                    $variants.Add([pscustomobject]@{ Needle = $n; Text = $enc.Substring($start, $full - $start); Kind = "b64-align$k"; Compare = [StringComparison]::Ordinal; Boundary = 'none' })
                }
            }
        }
    }
    return $variants
}

# ------------------------------------------------------------------ targets

function Add-Target {
    param($Targets, [string]$Name, [string]$Text, $LineNumbers, [string]$Scope)
    if ([string]::IsNullOrEmpty($Text)) { return }
    $Targets.Add([pscustomobject]@{ Name = $Name; Text = $Text; Lines = $LineNumbers; Scope = $Scope })
}

function Get-LineOf($Target, [int]$Offset) {
    $line = $Target.Text.Substring(0, $Offset).Split("`n").Length
    if ($null -ne $Target.Lines -and @($Target.Lines).Count -gt 0) { return @($Target.Lines)[[math]::Min($line, @($Target.Lines).Count) - 1] }
    return $line
}

# What kind of image a byte string starts as, or $null.
function Get-ImageKind([byte[]]$Bytes) {
    if ($Bytes.Length -ge 3 -and $Bytes[0] -eq 0xFF -and $Bytes[1] -eq 0xD8 -and $Bytes[2] -eq 0xFF) { return 'jpeg' }
    if ($Bytes.Length -ge 4 -and $Bytes[0] -eq 0x89 -and $Bytes[1] -eq 0x50 -and $Bytes[2] -eq 0x4E -and $Bytes[3] -eq 0x47) { return 'png' }
    if ($Bytes.Length -ge 4 -and $Bytes[0] -eq 0x47 -and $Bytes[1] -eq 0x49 -and $Bytes[2] -eq 0x46 -and $Bytes[3] -eq 0x38) { return 'gif' }
    if ($Bytes.Length -ge 4 -and (($Bytes[0] -eq 0x49 -and $Bytes[1] -eq 0x49 -and $Bytes[2] -eq 0x2A -and $Bytes[3] -eq 0x00) -or ($Bytes[0] -eq 0x4D -and $Bytes[1] -eq 0x4D -and $Bytes[2] -eq 0x00 -and $Bytes[3] -eq 0x2A))) { return 'tiff' }
    if ($Bytes.Length -ge 12 -and $script:Latin1.GetString($Bytes, 0, 4) -eq 'RIFF' -and $script:Latin1.GetString($Bytes, 8, 4) -eq 'WEBP') { return 'webp' }
    return $null
}

# SHA-256 (lower case hex) of the dummy images that sit in the tree at Head as images/*.jpg.b64 and
# pass the same test as a .b64 file in the range (small, with the synthetic-image notice). A decoded
# image with one of these hashes is a fixture dummy, not a photograph, wherever it is embedded.
function Get-HeadDummyImageHashes {
    param([string]$Root, [string]$Head)
    $hashes = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($p in @(Invoke-Git $Root @('ls-tree', '-r', '--name-only', $Head))) {
        if (([string]$p) -notmatch '(^|/)images/[^/]+\.jpg\.b64$') { continue }
        $content = (Invoke-Git $Root @('show', '--no-textconv', '--no-ext-diff', "${Head}:$p")) -join ''
        try { $bytes = [Convert]::FromBase64String(($content -replace '\s', '')) } catch { continue }
        if ((Get-ImageKind $bytes) -and (Test-DummyImage $bytes)) {
            $null = $hashes.Add([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)))
        }
    }
    return , $hashes
}

# A small image that carries the synthetic-image notice.
function Test-DummyImage([byte[]]$Bytes) {
    return ($Bytes.Length -le $script:SyntheticImageMaxBytes) -and $script:Latin1.GetString($Bytes).Contains($script:SyntheticImageNotice)
}

function Add-DecodedBytesTargets {
    param($Targets, $Structural, [string]$Name, [string]$Kind, [byte[]]$Bytes, [bool]$NoImageCheck)
    if (-not $NoImageCheck -and $Bytes.Length -ge $script:MinEmbeddedImageBytes -and (Get-ImageKind $Bytes)) {
        $sha = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes))
        if (-not $script:AllowedImageSha256.Contains($sha)) {
            $Structural.Add([pscustomobject]@{ Scope = 'embedded-image'; Target = $Name; Line = 0; Label = 'image' })
        }
    }
    $latin1 = $script:Latin1.GetString($Bytes)
    Add-Target $Targets "$Kind-latin1 $Name" $latin1 $null "decoded-$Kind"
    Add-Target $Targets "$Kind-utf16le $Name" ($script:Utf16.GetString($Bytes)) $null "decoded-$Kind"
    if ($Bytes.Length -gt 1) { Add-Target $Targets "$Kind-utf16le+1 $Name" ($script:Utf16.GetString($Bytes, 1, $Bytes.Length - 1)) $null "decoded-$Kind" }
    # UTF-8 reads differently from Latin-1 only when a high byte is present.
    if ([regex]::IsMatch($latin1, '[\x80-\xFF]')) {
        Add-Target $Targets "$Kind-utf8 $Name" ($script:Utf8.GetString($Bytes)) $null "decoded-$Kind"
    }
}

function Add-DecodedTargets {
    # Hex and base64 strings found in published text are decoded and compared as text too. Hex
    # may be written with separators (spaces, commas, colons, hyphens) or as 0x.. / \x.. arrays.
    param($Targets, $Structural, [string]$Name, [string]$Text, [bool]$NoImageCheck = $false)
    foreach ($m in [regex]::Matches($Text, '(?<![0-9A-Za-z])(?:(?:0[xX]|\\x)?[0-9a-fA-F]{2}[\s,;:\-]*){8,}')) {
        $hex = -join ([regex]::Matches($m.Value, '(?:0[xX]|\\x)?([0-9a-fA-F]{2})') | ForEach-Object { $_.Groups[1].Value })
        if ($hex.Length -lt 16) { continue }
        Add-DecodedBytesTargets $Targets $Structural "hex $Name" 'hex' ([Convert]::FromHexString($hex)) $NoImageCheck
    }
    foreach ($m in [regex]::Matches($Text, '(?<![A-Za-z0-9+/\-_])[A-Za-z0-9+/\-_]{16,}={0,2}(?![A-Za-z0-9+/=\-_])')) {
        $s = $m.Value.TrimEnd('=').Replace('-', '+').Replace('_', '/')
        $s = $s.PadRight($s.Length + ((4 - $s.Length % 4) % 4), '=')
        try { $bytes = [Convert]::FromBase64String($s) } catch { continue }
        Add-DecodedBytesTargets $Targets $Structural "b64 $Name" 'b64' $bytes $NoImageCheck
    }
}

# JSON, URL and HTML escapes restored (A, \x41, \/, %41, &#65;). A value written escaped is
# the same value for a reader of the file.
function Restore-Escapes {
    param([string]$Text)
    $t = $Text
    $t = [regex]::Replace($t, '\\u([0-9a-fA-F]{4})', { param($m) [string][char][Convert]::ToInt32($m.Groups[1].Value, 16) })
    $t = [regex]::Replace($t, '\\x([0-9a-fA-F]{2})', { param($m) [string][char][Convert]::ToInt32($m.Groups[1].Value, 16) })
    $t = [regex]::Replace($t, '\\([\\"/bfnrt])', { param($m) switch -CaseSensitive ($m.Groups[1].Value) { '\' { '\' } '"' { '"' } '/' { '/' } default { '' } } })
    $t = [regex]::Replace($t, '&#[xX]([0-9a-fA-F]{1,6});', { param($m) try { [char]::ConvertFromUtf32([Convert]::ToInt32($m.Groups[1].Value, 16)) } catch { '' } })
    $t = [regex]::Replace($t, '&#(\d{1,7});', { param($m) try { [char]::ConvertFromUtf32([int]$m.Groups[1].Value) } catch { '' } })
    try { $t = [Uri]::UnescapeDataString($t) } catch { }
    # Named HTML entities (&amp; &lt; ...) as well.
    $t = [System.Net.WebUtility]::HtmlDecode($t)
    return $t
}

# True when line $Index follows line $Index-1 in the file (line numbers known) or when numbers are unknown.
function Test-NextLine($LineNumbers, [int]$Index) {
    if ($null -eq $LineNumbers -or @($LineNumbers).Count -le $Index) { return $true }
    return (@($LineNumbers)[$Index] -eq (@($LineNumbers)[$Index - 1] + 1))
}

# Base64 written over several lines of one width (the last one shorter): each block of such lines is
# one base64 string. The lines before and after the block (a heading, a fence) do not shift it.
function Get-WrappedBase64Blocks {
    param([string[]]$Lines, $LineNumbers)
    $blocks = [System.Collections.Generic.List[string]]::new()
    $t = @($Lines | ForEach-Object { $_.Trim() })
    $n = $t.Count
    $i = 0
    while ($i -lt $n) {
        $len = $t[$i].Length
        if ($len -lt 8 -or $len % 4 -ne 0 -or $t[$i] -notmatch '^[A-Za-z0-9+/\-_]+$') { $i++; continue }
        $end = $i
        while ($end + 1 -lt $n -and (Test-NextLine $LineNumbers ($end + 1)) -and $t[$end + 1].Length -eq $len -and $t[$end + 1] -match '^[A-Za-z0-9+/\-_]+={0,2}$') {
            $end++
            if ($t[$end].EndsWith('=')) { break }
        }
        if (-not $t[$end].EndsWith('=') -and $end + 1 -lt $n -and (Test-NextLine $LineNumbers ($end + 1)) -and $t[$end + 1].Length -lt $len -and $t[$end + 1] -match '^[A-Za-z0-9+/\-_]+={0,2}$') { $end++ }
        if ($end -gt $i) { $blocks.Add(($t[$i..$end] -join '')) }
        $i = $end + 1
    }
    return , $blocks
}

# The hex bytes of one line of a hex dump (hexdump -C, xxd, Format-Hex: an address, the bytes, then a
# character column of one character per byte), or $null when the line is no dump line. Address and
# character column are not part of the value.
function Get-HexDumpLineBytes([string]$Line) {
    $m = [regex]::Match($Line, '^\s*(?:0x)?[0-9A-Fa-f]{4,16}h?:?[ \t]+(?<rest>\S.*)$')
    if (-not $m.Success) { return $null }
    $rest = $m.Groups['rest'].Value.TrimEnd("`r")
    $bar = $rest.IndexOf('|')
    if ($bar -ge 0) { $rest = $rest.Substring(0, $bar) }
    $tokens = @([regex]::Matches($rest, '\S+'))
    $hexTokens = 0
    while ($hexTokens -lt $tokens.Count -and $tokens[$hexTokens].Value -match '^(?:[0-9A-Fa-f]{2})+$') { $hexTokens++ }
    if ($hexTokens -eq 0) { return $null }
    # With a character column, take the shortest run of tokens whose remainder is exactly one character per byte.
    $take = $hexTokens
    $bytes = 0
    for ($k = 1; $k -le $hexTokens; $k++) {
        $bytes += $tokens[$k - 1].Value.Length / 2
        $after = $tokens[$k - 1].Index + $tokens[$k - 1].Length
        $column = $rest.Substring($after).TrimStart()
        if ($column.Length -gt 0 -and $column.Length -eq $bytes) { $take = $k; break }
    }
    return -join ($tokens[0..($take - 1)] | ForEach-Object { $_.Value })
}

# Consecutive hex dump lines are one byte string.
function ConvertFrom-HexDumpLines {
    param([string[]]$Lines, $LineNumbers)
    $results = [System.Collections.Generic.List[string]]::new()
    $current = [System.Text.StringBuilder]::new()
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $hex = Get-HexDumpLineBytes $Lines[$i]
        if ($null -ne $hex -and ($current.Length -eq 0 -or (Test-NextLine $LineNumbers $i))) { $null = $current.Append($hex); continue }
        if ($current.Length -gt 0) { $results.Add($current.ToString()); $null = $current.Clear() }
        if ($null -ne $hex) { $null = $current.Append($hex) }
    }
    if ($current.Length -gt 0) { $results.Add($current.ToString()) }
    return , $results
}

# The published text as written, and in normalized forms: escapes restored, and the lines joined
# without line breaks and indentation (a value wrapped over lines, base64 wrapped at 76 columns).
function Add-TextTargets {
    param($Targets, $Structural, [string]$Name, [string[]]$Lines, $LineNumbers, [string]$Scope, [string]$DecodedName, [bool]$NoImageCheck = $false)
    $text = $Lines -join "`n"
    Add-Target $Targets "$Scope $Name" $text $LineNumbers $Scope
    Add-DecodedTargets $Targets $Structural $DecodedName $text $NoImageCheck
    $first = $null
    if ($null -ne $LineNumbers -and @($LineNumbers).Count -gt 0) { $first = @(@($LineNumbers)[0]) }
    $joined = (@($Lines | ForEach-Object { $_.Trim() })) -join ''
    $forms = [ordered]@{
        'unescaped'        = (Restore-Escapes $text)
        'joined'           = $joined
        'joined-unescaped' = (Restore-Escapes $joined)
    }
    $done = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $null = $done.Add($text)
    foreach ($k in $forms.Keys) {
        $v = [string]$forms[$k]
        if (-not $done.Add($v)) { continue }
        Add-Target $Targets "normalized-$k $Name" $v $first 'normalized'
        Add-DecodedTargets $Targets $Structural "$k $DecodedName" $v $NoImageCheck
    }
    # Base64 wrapped over lines is one string, apart from the lines around it.
    foreach ($block in (Get-WrappedBase64Blocks $Lines $LineNumbers)) {
        $s = $block.TrimEnd('=').Replace('-', '+').Replace('_', '/')
        $s = $s.PadRight($s.Length + ((4 - $s.Length % 4) % 4), '=')
        try { $bytes = [Convert]::FromBase64String($s) } catch { continue }
        Add-DecodedBytesTargets $Targets $Structural "b64block $DecodedName" 'b64block' $bytes $NoImageCheck
    }
    # A hex dump (address, bytes, character column) is one byte string, without the address and the characters.
    foreach ($hex in (ConvertFrom-HexDumpLines $Lines $LineNumbers)) {
        if ($hex.Length -lt 16) { continue }
        Add-DecodedBytesTargets $Targets $Structural "hexdump $DecodedName" 'hexdump' ([Convert]::FromHexString($hex)) $NoImageCheck
    }
}

function Invoke-Git {
    param([string]$Root, [string[]]$GitArgs)
    # --no-replace-objects: a refs/replace entry would make git show other content than the push
    # publishes (a push sends the real objects, not the replacements).
    $out = & git --no-replace-objects -C $Root -c core.quotepath=off @GitArgs 2>&1
    # The message is fixed text: git's own output can hold paths and the revisions given.
    if ($LASTEXITCODE -ne 0) { throw (New-SafeError "git $($GitArgs[0]) failed") }
    return $out
}

function Get-PublishTargets {
    param([string]$Root, [string]$Base, [string]$Head, [bool]$IncludeChangedFiles)
    $targets = [System.Collections.Generic.List[object]]::new()
    $structural = [System.Collections.Generic.List[object]]::new()
    $script:AllowedImageSha256 = Get-HeadDummyImageHashes $Root $Head
    $commits = @(Invoke-Git $Root @('rev-list', '--reverse', "$Base..$Head"))
    Write-Host "commits in range $Base..$Head : $($commits.Count)"
    $merges = @(Invoke-Git $Root @('rev-list', '--merges', "$Base..$Head"))
    foreach ($c in $merges) { $structural.Add([pscustomobject]@{ Scope = 'merge-commit'; Target = $c.Substring(0, 7); Line = 0; Label = 'merge' }) }

    foreach ($c in $commits) {
        $short = $c.Substring(0, 7)
        $message = (Invoke-Git $Root @('log', '-1', '--format=%B', $c)) -join "`n"
        Add-TextTargets $targets $structural "$short" ($message -split "`n") $null 'commit-message' "commit-message $short"

        # Author and committer (name and e-mail) are published with every commit.
        $identity = @(Invoke-Git $Root @('log', '-1', '--format=%an%n%ae%n%cn%n%ce', $c))
        Add-Target $targets "identity $short" ($identity -join "`n") $null 'author-committer'

        foreach ($line in (Invoke-Git $Root @('diff-tree', '--root', '--no-commit-id', '--numstat', '-r', $c))) {
            if ($line -match "^-`t-`t(.+)$") { $structural.Add([pscustomobject]@{ Scope = 'binary-file'; Target = "$short $($Matches[1])"; Line = 0; Label = 'binary' }) }
        }

        # Every path the commit touches; for a rename both the old and the new path.
        $nameStatus = @(Invoke-Git $Root @('diff-tree', '--root', '-r', '-M', '--no-commit-id', '--name-status', $c))
        $paths = [System.Collections.Generic.List[string]]::new()
        $b64Paths = [System.Collections.Generic.List[string]]::new()
        foreach ($entry in $nameStatus) {
            $fields = ([string]$entry) -split "`t"
            if ($fields.Count -lt 2) { continue }
            foreach ($p in $fields[1..($fields.Count - 1)]) { $paths.Add($p) }
            if ($fields[0] -match '^[AMRC]' -and $fields[-1] -like '*.b64') { $b64Paths.Add($fields[-1]) }
        }
        Add-Target $targets "path $short" ($paths -join "`n") $null 'file-path'

        $diff = (Invoke-Git $Root @('show', '--no-color', '--no-textconv', '--no-ext-diff', '--format=', '--unified=0', $c)) -join "`n"
        $file = $null; $newLine = 0; $inHunk = $false
        $buf = [System.Collections.Generic.List[string]]::new(); $nums = [System.Collections.Generic.List[int]]::new()
        $flush = {
            if ($file -and $buf.Count -gt 0) {
                Add-TextTargets $targets $structural "$short $file" $buf.ToArray() $nums.ToArray() 'added-line' "$short $file" ($file -like '*.b64')
            }
        }
        foreach ($line in $diff -split "`n") {
            if ($line.StartsWith('diff --git ')) {
                & $flush
                $buf = [System.Collections.Generic.List[string]]::new(); $nums = [System.Collections.Generic.List[int]]::new()
                $file = ($line -replace '^diff --git a/.* b/', ''); $inHunk = $false; continue
            }
            if ($line -match '^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@') { $newLine = [int]$Matches[1]; $inHunk = $true; continue }
            if (-not $inHunk) { continue }
            if ($line.StartsWith('+')) { $buf.Add($line.Substring(1)); $nums.Add($newLine); $newLine++ }
        }
        & $flush

        # .b64 files are multi-line: decode the whole file as of this commit. A file that decodes
        # to an image is accepted only as a small dummy that carries the synthetic-image notice.
        foreach ($p in $b64Paths) {
            $content = (Invoke-Git $Root @('show', '--no-textconv', '--no-ext-diff', "${c}:$p")) -join ''
            try { $bytes = [Convert]::FromBase64String(($content -replace '\s', '')) } catch {
                $structural.Add([pscustomobject]@{ Scope = 'unreadable-b64'; Target = "$short $p"; Line = 0; Label = 'b64' }); continue
            }
            if (Get-ImageKind $bytes) {
                $dummy = Test-DummyImage $bytes
                if (-not $dummy) { $structural.Add([pscustomobject]@{ Scope = 'embedded-image'; Target = "$short $p"; Line = 0; Label = 'image' }) }
            }
            Add-Target $targets "b64-file-latin1 $short $p" ($script:Latin1.GetString($bytes)) $null 'decoded-b64'
            Add-Target $targets "b64-file-utf16le $short $p" ($script:Utf16.GetString($bytes)) $null 'decoded-b64'
            if ($bytes.Length -gt 1) { Add-Target $targets "b64-file-utf16le+1 $short $p" ($script:Utf16.GetString($bytes, 1, $bytes.Length - 1)) $null 'decoded-b64' }
        }
    }

    if ($IncludeChangedFiles -and $commits.Count -gt 0) {
        # Three dots: what the push adds on top of the common ancestor. Two dots would also list the
        # files that only Base changed, and read files that are not part of this push.
        foreach ($p in @(Invoke-Git $Root @('diff', '--no-textconv', '--no-ext-diff', '--name-only', '--diff-filter=AM', "$Base...$Head"))) {
            $full = Join-Path $Root $p
            if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }
            $bytes = [IO.File]::ReadAllBytes($full)
            if ($bytes.Length -gt 3MB -or [Array]::IndexOf($bytes, [byte]0) -ge 0) { continue }
            Add-Target $targets "tree $p" ($script:Utf8.GetString($bytes)) $null 'changed-file'
        }
    }
    return [pscustomobject]@{ Targets = $targets; Structural = $structural; CommitCount = $commits.Count }
}

function Test-MatchBoundary([string]$Text, [int]$Index, [int]$Length, [string]$Mode) {
    if ($Mode -eq 'none') { return $true }
    $end = $Index + $Length
    if ($Mode -eq 'digit') {
        # Digits in front count as another number, unless they are all zeros: a value padded with
        # zeros to another width is the same value.
        $k = $Index - 1
        while ($k -ge 0 -and [char]::IsDigit($Text[$k])) {
            if ($Text[$k] -ne '0') { return $false }
            $k--
        }
        if ($end -lt $Text.Length -and [char]::IsDigit($Text[$end])) { return $false }
        return $true
    }
    # 'word': not inside a longer word
    if ($Index -gt 0 -and ([char]::IsLetterOrDigit($Text[$Index - 1]) -or $Text[$Index - 1] -eq '_')) { return $false }
    if ($end -lt $Text.Length -and ([char]::IsLetterOrDigit($Text[$end]) -or $Text[$end] -eq '_')) { return $false }
    return $true
}

function Find-NeedleHits {
    param($Variants, $Targets, $Hits)
    $seen = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($t in $Targets) {
        foreach ($v in $Variants) {
            $idx = $t.Text.IndexOf($v.Text, $v.Compare)
            while ($idx -ge 0) {
                if (Test-MatchBoundary $t.Text $idx $v.Text.Length $v.Boundary) {
                    $line = Get-LineOf $t $idx
                    if ($seen.Add("$($t.Scope)|$($t.Name)|$line|$($v.Needle.Label)|$($v.Kind)")) {
                        $Hits.Add([pscustomobject]@{ Scope = $t.Scope; Target = $t.Name; Line = $line; Label = $v.Needle.Label; Kind = $v.Kind })
                    }
                }
                $idx = $t.Text.IndexOf($v.Text, $idx + 1, $v.Compare)
            }
        }
    }
}

# A location printed for a hit holds a repository path, and the path itself may hold a real value.
# Every part of the text that is one of the needle forms is replaced by asterisks (wherever it
# sits, without the boundary rules of the comparison: hiding too much is the safe side).
function Hide-NeedleText {
    param([string]$Text, $Variants)
    if ([string]::IsNullOrEmpty($Text)) { return $Text }
    $spans = [System.Collections.Generic.List[int[]]]::new()
    foreach ($v in $Variants) {
        if ([string]::IsNullOrEmpty($v.Text)) { continue }
        $idx = $Text.IndexOf($v.Text, $v.Compare)
        while ($idx -ge 0) {
            $spans.Add(@($idx, $v.Text.Length))
            $idx = $Text.IndexOf($v.Text, $idx + 1, $v.Compare)
        }
    }
    if ($spans.Count -eq 0) { return $Text }
    $mask = [bool[]]::new($Text.Length)
    foreach ($s in $spans) { for ($i = $s[0]; $i -lt $s[0] + $s[1]; $i++) { $mask[$i] = $true } }
    $sb = [System.Text.StringBuilder]::new()
    for ($i = 0; $i -lt $Text.Length; $i++) {
        if ($mask[$i]) { if ($i -eq 0 -or -not $mask[$i - 1]) { $null = $sb.Append('***') } }
        else { $null = $sb.Append($Text[$i]) }
    }
    return $sb.ToString()
}

# ------------------------------------------------------------------ main check

function Invoke-LeakCheck {
    param(
        [string]$Root, [string]$Base, [string]$Head, [string]$SourceRoot, [string[]]$ExtraNeedle,
        [string[]]$ExtraPhotoRoot, [bool]$IncludeChangedFiles, [bool]$NoLocalSources,
        [bool]$SelfTestIsolation = $false)

    $set = New-NeedleSet
    $cannotVerify = $null
    $recordBodies = 0
    if (-not $NoLocalSources) {
        $note = Get-SourceRootNote $SourceRoot
        if ($note) { Write-Host $note }
        if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
            $cannotVerify = "The local source data folder does not exist: nothing real to compare against."
        }
        else {
            $local = Get-LocalNeedles $SourceRoot $ExtraPhotoRoot $SelfTestIsolation $SelfTestIsolation
            $set = $local.Set
            $recordBodies = $local.RecordBodies
            Write-Host "local sources: record files=$($local.SourceFiles), photographs=$($local.Photos) (export folders=$($local.ExportDirs), photographs there=$($local.ExportPhotos)), nikon usb instances=$($local.UsbInstances), wpd keys=$($local.WpdKeys), camera bodies named in the records=$recordBodies"
            if ($local.SourceFiles -eq 0) { $cannotVerify = 'The local source data folder holds no files.' }
            elseif ($local.Capped) { $cannotVerify = "A photograph folder holds more than $($script:MaxPhotosPerRoot) photographs and was not read completely." }
            elseif ((Get-RecordIdCount $set) -eq 0) {
                # Without any ID from the records the comparison would cover only the PC and the photographs.
                $cannotVerify = 'No ID (32 or 64 digit hex, run ID, approval reference) could be read from the records.'
            }
        }
    }
    foreach ($v in $ExtraNeedle) { Add-Needle $set $v 'extra' }

    Write-Host '=== needle inventory (counts per category; values withheld) ==='
    $set.List | Group-Object Category | Sort-Object Name | ForEach-Object { Write-Host ('{0,-26} {1}' -f $_.Name, $_.Count) }
    foreach ($k in $set.Skipped.Keys) { Write-Host ('skipped (short plain word): {0} x{1}' -f $k, $set.Skipped[$k]) }
    $bodySerials = Get-BodySerialCount $set
    Write-Host "body serial needles (EXIF, USB, WPD) = $bodySerials"
    if (-not $NoLocalSources -and -not $cannotVerify -and $bodySerials -eq 0) {
        # Without a body serial number the check would pass a push that carries one.
        $cannotVerify = 'No body serial number could be read (no photograph with EXIF, no Nikon USB or WPD registry entry on this PC).'
    }
    if (-not $NoLocalSources -and -not $cannotVerify) {
        # A dual-camera record names two bodies: one serial would leave the other body unchecked.
        $distinctBodies = Get-DistinctBodyCount $set
        Write-Host "distinct body serials = $distinctBodies (camera bodies named in the records = $recordBodies)"
        if ($recordBodies -ge 2 -and $distinctBodies -lt $recordBodies) {
            $cannotVerify = "The records name $recordBodies camera bodies, but only $distinctBodies distinct body serial number(s) could be read."
        }
    }
    if ($set.List.Count -eq 0 -and -not $cannotVerify) { $cannotVerify = 'No needles could be built.' }
    if ($cannotVerify) {
        Write-Host "CANNOT VERIFY: $cannotVerify Do not push."
        return [pscustomobject]@{ Exit = 2; Hits = @(); Needles = $set.List.Count; BodySerials = $bodySerials }
    }

    $variants = New-Variants $set
    Write-Host "needles=$($set.List.Count) variants=$($variants.Count)"
    $published = Get-PublishTargets $Root $Base $Head $IncludeChangedFiles
    Write-Host "targets=$($published.Targets.Count)"

    $hits = [System.Collections.Generic.List[object]]::new()
    foreach ($s in @($published.Structural | Sort-Object Scope, Target, Label -Unique)) { $hits.Add($s) }
    Find-NeedleHits $variants $published.Targets $hits

    Write-Host '=== HITS (labels and locations only; values withheld) ==='
    foreach ($g in $hits | Group-Object Scope) {
        Write-Host "## scope=$($g.Name) hits=$($g.Count)"
        foreach ($h in ($g.Group | Sort-Object Target, Line | Select-Object -First 80)) {
            $where = if ($h.Line -gt 0) { " :$($h.Line)" } else { '' }
            Write-Host ('  {0}{1}  {2} ({3})' -f (Hide-NeedleText $h.Target $variants), $where, $h.Label, $(if ($h.PSObject.Properties['Kind']) { $h.Kind } else { '-' }))
        }
    }
    if ($hits.Count -eq 0) { Write-Host 'NO HITS in the content this push would publish.' }
    Write-Host "=== SUMMARY: hits in content this push would publish = $($hits.Count)"
    return [pscustomobject]@{ Exit = $(if ($hits.Count -eq 0) { 0 } else { 1 }); Hits = $hits; Needles = $set.List.Count; BodySerials = $bodySerials }
}

# One run of the check as the command line asks for it. Returns the exit code; never throws, and
# prints no path and no exception message (only a fixed sentence and the kind of failure).
function Invoke-Entry {
    param(
        [string]$Base, [string]$Head, [string]$RepositoryRoot, [string]$SourceRoot, [string[]]$ExtraNeedle,
        [string[]]$ExtraPhotoRoot, [bool]$IncludeChangedFiles, [bool]$NoLocalSources, [bool]$InSelfTest)
    try {
        if ($NoLocalSources -and -not $InSelfTest) {
            Write-Host 'CANNOT VERIFY: -NoLocalSources is only valid together with -SelfTest. Do not push.'
            return 2
        }
        if ([string]::IsNullOrWhiteSpace($Base)) {
            & git -C $RepositoryRoot rev-parse --verify --quiet '@{u}' *> $null
            $Base = if ($LASTEXITCODE -eq 0) { '@{u}' } else { 'origin/main' }
        }
        Write-Host "base=$Base head=$Head"
        $result = Invoke-LeakCheck -Root $RepositoryRoot -Base $Base -Head $Head -SourceRoot $SourceRoot -ExtraNeedle $ExtraNeedle `
            -ExtraPhotoRoot $ExtraPhotoRoot -IncludeChangedFiles $IncludeChangedFiles -NoLocalSources $NoLocalSources
        return $result.Exit
    }
    catch {
        $safe = $_.Exception.Data['SafeMessage']
        if ($safe) { Write-Host "CANNOT VERIFY: $safe. Do not push." }
        else { Write-Host "CANNOT VERIFY: the check stopped on an error of kind $($_.Exception.GetType().Name). Do not push." }
        return 2
    }
}

# ------------------------------------------------------------------ self test (invented values only)

# A JPEG with just enough structure for the EXIF reader: APP1 with a body serial number
# (BodySerialNumber, tag A431) in the Exif IFD. The value is invented.
function New-SelfTestJpeg {
    param([string]$Serial)
    $ascii = [byte[]](@($script:Latin1.GetBytes($Serial)) + @([byte]0))
    $tiff = [System.Collections.Generic.List[byte]]::new()
    $tiff.AddRange([byte[]](0x49, 0x49, 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00))             # II, 42, IFD0 at 8
    $tiff.AddRange([byte[]](0x01, 0x00, 0x69, 0x87, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00))  # 1 entry: ExifIFD at 26
    $tiff.AddRange([byte[]](0x00, 0x00, 0x00, 0x00))                                       # no next IFD
    $tiff.AddRange([byte[]](0x01, 0x00, 0x31, 0xA4, 0x02, 0x00))                           # Exif IFD: 1 entry, A431, ASCII
    $tiff.AddRange([byte[]]([BitConverter]::GetBytes([int]$ascii.Length)))
    $tiff.AddRange([byte[]]([BitConverter]::GetBytes([int]44)))                            # value offset
    $tiff.AddRange([byte[]](0x00, 0x00, 0x00, 0x00))
    $tiff.AddRange($ascii)
    $payload = [byte[]](@(0x45, 0x78, 0x69, 0x66, 0x00, 0x00) + $tiff.ToArray())
    $len = $payload.Length + 2
    return [byte[]](@(0xFF, 0xD8, 0xFF, 0xE1, [byte]($len -shr 8), [byte]($len -band 0xFF)) + $payload + @(0xFF, 0xD9))
}

# Runs the check with the given arguments and returns its result and what it printed.
function Invoke-CheckCaptured {
    param([hashtable]$Arguments)
    $all = @(Invoke-LeakCheck @Arguments 6>&1)
    $info = @($all | Where-Object { $_ -is [System.Management.Automation.InformationRecord] })
    $result = @($all | Where-Object { $_ -isnot [System.Management.Automation.InformationRecord] }) | Select-Object -Last 1
    return [pscustomobject]@{ Result = $result; Text = ($info | Out-String) }
}

function Invoke-SelfTestGit {
    param([string]$Dir, [string[]]$GitArgs)
    $o = & git -C $Dir @GitArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs[0]) failed: $($o -join ' ')" }
    return $o
}

# A throw-away repository of its own (invented identity, no signing, no line-ending conversion).
function New-SelfTestRepo {
    param([string]$Dir)
    New-Item -ItemType Directory -Path $Dir | Out-Null
    foreach ($a in @(@('init', '-q'), @('config', 'user.name', 'selftest'), @('config', 'user.email', 'selftest@example.invalid'),
            @('config', 'core.autocrlf', 'false'), @('config', 'commit.gpgsign', 'false'))) {
        $null = Invoke-SelfTestGit $Dir $a
    }
}

function Add-SelfTestCommit {
    param([string]$Dir, [string]$Message)
    $null = Invoke-SelfTestGit $Dir @('add', '-A')
    $null = Invoke-SelfTestGit $Dir @('commit', '-q', '-m', $Message)
    return (Invoke-SelfTestGit $Dir @('rev-parse', 'HEAD'))
}

function Invoke-SelfTest {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("a0-leak-selftest-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    $needle = 'zz-selftest-needle-0001'
    $digits = '7654321'
    $failures = [System.Collections.Generic.List[string]]::new()
    try {
        $git = { $o = & git -C $tmp @args 2>&1; if ($LASTEXITCODE -ne 0) { throw "git $($args[0]) failed: $($o -join ' ')" } }
        & $git init -q
        & $git config user.name selftest
        & $git config user.email selftest@example.invalid
        & $git config core.autocrlf false
        & $git config commit.gpgsign false
        function Commit([string]$Message, $Identity = $null) {
            & $git add -A
            $saved = @{}
            if ($Identity) { foreach ($k in $Identity.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); Set-Item -LiteralPath "Env:$k" -Value $Identity[$k] } }
            try { & $git commit -q -m $Message } finally { foreach ($k in $saved.Keys) { if ($null -eq $saved[$k]) { Remove-Item -LiteralPath "Env:$k" -ErrorAction SilentlyContinue } else { Set-Item -LiteralPath "Env:$k" -Value $saved[$k] } } }
            return (& git -C $tmp rev-parse HEAD)
        }
        $write = { param([string]$Rel, [string]$Text) $p = Join-Path $tmp $Rel; New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null; [IO.File]::WriteAllText($p, $Text, $script:Utf8) }
        $scenarios = [System.Collections.Generic.List[object]]::new()
        $add = {
            param([string]$Name, [scriptblock]$Setup, [string]$Message, [bool]$ExpectHit, $Identity = $null)
            & $Setup
            $c = Commit $Message $Identity
            $scenarios.Add([pscustomobject]@{ Name = $Name; Base = $script:prev; Head = $c; ExpectHit = $ExpectHit })
            $script:prev = $c
        }
        & $write 'README.txt' 'seed'
        $script:prev = Commit 'seed'

        & $add 'plain added line' { & $write 'a.txt' "line one`nvalue is $needle here`n" } 'add a' $true
        & $add 'needle removed again (this commit alone is clean)' { & $write 'a.txt' "line one`n" } 'remove needle' $false
        & $add 'upper case' { & $write 'b.txt' ($needle.ToUpperInvariant()) } 'add b' $true
        $hex = [Convert]::ToHexString($script:Utf8.GetBytes("x $needle y"))
        & $add 'hex of text in JSON' { & $write 'c.json' "{`"terminalResultHex`":`"$hex`"}`n" } 'add c' $true
        $b64 = [Convert]::ToBase64String($script:Utf8.GetBytes("QQ$needle"))
        & $add 'base64 file, wrapped' { & $write 'images/d.jpg.b64' ($b64.Substring(0, 8) + "`n" + $b64.Substring(8) + "`n") } 'add d' $true
        $b64inline = [Convert]::ToBase64String($script:Utf8.GetBytes("Q$needle"))
        & $add 'base64 inside JSON' { & $write 'e.json' "{`"v`":`"$b64inline`"}`n" } 'add e' $true
        & $add 'commit message' { & $write 'f.txt' 'clean' } "message mentions $needle" $true
        & $add 'binary file' { [IO.File]::WriteAllBytes((Join-Path $tmp 'g.bin'), [byte[]](1, 0, 2, 0, 255, 254, 0, 0)) } 'add binary' $true
        & $add 'digit needle inside longer number' { & $write 'h.txt' "id 1${digits}9 only`n" } 'add h' $false
        & $add 'digit needle standalone' { & $write 'i.txt' "id $digits only`n" } 'add i' $true
        & $add 'clean commit' { & $write 'j.txt' "nothing to see`n" } 'add j' $false

        # M-2: file paths (also the old path of a rename) and author / committer fields.
        & $add 'needle in a new file path' { & $write "k-$needle.txt" "stable line one`nstable line two`nstable line three`nstable line four`n" } 'add k' $true
        & $add 'rename away from a needle path (old path)' { & $git mv "k-$needle.txt" 'k-plain.txt' } 'rename k away' $true
        & $add 'rename to a needle path (new path)' { & $git mv 'k-plain.txt' "k2-$needle.txt" } 'rename k to' $true
        & $add 'needle in a directory name' { & $write "dir-$needle/l.txt" "clean`n" } 'add l' $true
        & $add 'clean commit after path scenarios' { & $write 'm.txt' "nothing to see`n" } 'add m' $false
        & $add 'needle in the author name' { & $write 'n1.txt' "clean`n" } 'add n1' $true @{ GIT_AUTHOR_NAME = $needle }
        & $add 'needle in the author e-mail' { & $write 'n2.txt' "clean`n" } 'add n2' $true @{ GIT_AUTHOR_EMAIL = "$needle@example.invalid" }
        & $add 'needle in the committer name' { & $write 'n3.txt' "clean`n" } 'add n3' $true @{ GIT_COMMITTER_NAME = $needle }
        & $add 'needle in the committer e-mail' { & $write 'n4.txt' "clean`n" } 'add n4' $true @{ GIT_COMMITTER_EMAIL = "$needle@example.invalid" }
        & $add 'clean identity' { & $write 'n5.txt' "clean`n" } 'add n5' $false

        # M-3: images inside text.
        $jpegBytes = [byte[]](@(0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10) + @(0x41) * 100 + @(0xFF, 0xD9))
        $jpegB64 = [Convert]::ToBase64String($jpegBytes)
        & $add 'JPEG as base64 under a *Base64 key' { & $write 'o1.json' "{`"frameJpegBase64`":`"$jpegB64`"}`n" } 'add o1' $true
        $pngB64 = [Convert]::ToBase64String([byte[]](@(0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A) + @(0x41) * 80))
        & $add 'PNG as base64 in prose' { & $write 'o2.md' "preview: $pngB64`n" } 'add o2' $true
        & $add 'JPEG as spaced hex' { & $write 'o3.txt' ((($jpegBytes | ForEach-Object { $_.ToString('x2') }) -join ' ') + "`n") } 'add o3' $true
        $textB64 = [Convert]::ToBase64String($script:Utf8.GetBytes('hello world, this is only text.'))
        & $add 'base64 of plain text without the needle' { & $write 'o4.json' "{`"v`":`"$textB64`"}`n" } 'add o4' $false
        $dummy = [byte[]](@(0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10) + @(0x41) * 14 + $script:Latin1.GetBytes($script:SyntheticImageNotice) + @(0xFF, 0xD9))
        & $add 'a small dummy image in a .b64 file' { & $write 'images/dummy.jpg.b64' ([Convert]::ToBase64String($dummy) + "`n") } 'add o5' $false
        $real = [byte[]](@(0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10) + @(0x41) * 600 + @(0xFF, 0xD9))
        & $add 'an image without the notice in a .b64 file' { & $write 'images/real.jpg.b64' ([Convert]::ToBase64String($real) + "`n") } 'add o6' $true
        $big = [byte[]](@(0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10) + @(0x41) * 2000 + @(0xFF, 0xD9))
        $bigWrapped = [Convert]::ToBase64String($big, [Base64FormattingOptions]::InsertLineBreaks)
        & $add 'JPEG wrapped at 76 columns after a heading line' { & $write 'o7.md' "Frame`n`n``````text`n$bigWrapped`n```````n" } 'add o7' $true

        # L6: an image whose SHA-256 is that of a dummy under images/*.jpg.b64 at Head is no hit, wherever
        # it is embedded; any other image still is.
        $dummyB64 = [Convert]::ToBase64String($dummy)
        & $add 'the fixture dummy image under a *Base64 key' { & $write 'q1.json' "{`"frameJpegBase64`":`"$dummyB64`"}`n" } 'add q1' $false
        $otherDummy = [byte[]]$dummy.Clone(); $otherDummy[10] = 0x42
        $otherDummyB64 = [Convert]::ToBase64String($otherDummy)
        & $add 'a dummy-like image with one other byte under a *Base64 key' { & $write 'q2.json' "{`"frameJpegBase64`":`"$otherDummyB64`"}`n" } 'add q2' $true
        $dummyHex = [Convert]::ToHexString($dummy)
        & $add 'the fixture dummy image as hex' { & $write 'q3.txt' "dump $dummyHex`n" } 'add q3' $false

        # L3: a value padded with zeros to another width is the same value.
        & $add 'digit needle padded with zeros' { & $write 'q4.txt' "id 000${digits} only`n" } 'add q4' $true
        & $add 'digit needle behind a non-zero digit' { & $write 'q5.txt' "id 90${digits} only`n" } 'add q5' $false
        & $add 'digit needle behind zeros and a non-zero digit' { & $write 'q6.txt' "id 0900${digits} only`n" } 'add q6' $false

        # M-5: the same value written in other ways.
        $hexSep = ($script:Utf8.GetBytes($needle) | ForEach-Object { $_.ToString('x2') }) -join ' '
        & $add 'hex with spaces' { & $write 'p1.txt' "bytes: $hexSep`n" } 'add p1' $true
        $hexColon = ($script:Utf8.GetBytes($needle) | ForEach-Object { $_.ToString('x2') }) -join ':'
        & $add 'hex with colons' { & $write 'p2.txt' "bytes $hexColon`n" } 'add p2' $true
        $hex0x = ($script:Utf8.GetBytes($needle) | ForEach-Object { '0x' + $_.ToString('x2') }) -join ', '
        & $add '0x array' { & $write 'p3.c' "unsigned char v[] = { $hex0x };`n" } 'add p3' $true
        $hexBs = ($script:Utf8.GetBytes($needle) | ForEach-Object { '\x' + $_.ToString('x2') }) -join ''
        & $add '\x escapes' { & $write 'p4.c' "char v[] = `"$hexBs`";`n" } 'add p4' $true
        $hex16 = [Convert]::ToHexString($script:Utf16.GetBytes($needle))
        & $add 'hex of UTF-16LE' { & $write 'p5.txt' "wide $hex16`n" } 'add p5' $true
        $hex16sep = ($script:Utf16.GetBytes($needle) | ForEach-Object { $_.ToString('x2') }) -join ' '
        & $add 'hex of UTF-16LE with spaces' { & $write 'p6.txt' "wide $hex16sep`n" } 'add p6' $true
        $uEsc = -join ($needle.ToCharArray() | ForEach-Object { '\u{0:x4}' -f [int]$_ })
        & $add 'JSON \u escapes' { & $write 'p7.json' "{`"v`":`"$uEsc`"}`n" } 'add p7' $true
        $pctEsc = -join ($script:Utf8.GetBytes($needle) | ForEach-Object { '%{0:x2}' -f $_ })
        & $add 'URL percent escapes' { & $write 'p8.txt' "http://example.invalid/?q=$pctEsc`n" } 'add p8' $true
        & $add 'value wrapped over two lines' { & $write 'p9.md' "first part zz-selftest-`n    needle-0001 and more`n" } 'add p9' $true
        $wrapped = [Convert]::ToBase64String($script:Utf8.GetBytes("QQ$needle"))
        & $add 'base64 wrapped at 8 columns' { & $write 'p10.md' ((($wrapped -split '(?<=\G.{8})(?!$)') -join "`n") + "`n") } 'add p10' $true
        # L5: hex dumps (the address and the character column sit between the bytes), HTML entities.
        $dumpRows = {
            param([byte[]]$Bytes, [string]$Style)
            for ($o = 0; $o -lt $Bytes.Length; $o += 16) {
                $chunk = $Bytes[$o..([math]::Min($o + 15, $Bytes.Length - 1))]
                $ascii = -join ($chunk | ForEach-Object { if ($_ -ge 32 -and $_ -lt 127) { [string][char]$_ } else { '.' } })
                $pairs = @(0..15 | ForEach-Object { if ($_ -lt $chunk.Length) { '{0:x2}' -f $chunk[$_] } else { '  ' } })
                switch ($Style) {
                    'hexdump' { '{0:x8}  {1}  {2}  |{3}|' -f $o, ($pairs[0..7] -join ' '), ($pairs[8..15] -join ' '), $ascii }
                    'xxd' { '{0:x8}: {1}  {2}' -f $o, ((0..7 | ForEach-Object { ($pairs[2 * $_] + $pairs[2 * $_ + 1]).TrimEnd() } | Where-Object { $_ }) -join ' '), $ascii }
                    'formathex' { '{0:x8}   {1}   {2}' -f $o, ($pairs -join ' '), $ascii }
                }
            }
        }
        $dumpBytes = $script:Utf8.GetBytes("x $needle y")
        foreach ($style in 'hexdump', 'xxd', 'formathex') {
            $rows = @(& $dumpRows $dumpBytes $style)
            & $add "needle in a hex dump over two lines ($style)" { & $write "r-$style.txt" (($rows -join "`n") + "`n") } "add r $style" $true
        }
        $cleanRows = @(& $dumpRows $script:Utf8.GetBytes('hello world, this is a clean sentence for the dump.') 'hexdump')
        & $add 'a hex dump of clean text' { & $write 'r4.txt' (($cleanRows -join "`n") + "`n") } 'add r4' $false
        $jpegRows = @(& $dumpRows $jpegBytes 'xxd')
        & $add 'JPEG as a hex dump' { & $write 'r5.txt' (($jpegRows -join "`n") + "`n") } 'add r5' $true
        & $add 'needle behind an HTML entity (&amp;)' { & $write 'r6.html' "<p>value zz&amp;tail-needle-0002 here</p>`n" } 'add r6' $true
        $wrap16 = ($jpegB64 -split '(?<=\G.{16})(?!$)') -join "`n"
        & $add 'JPEG wrapped at 16 columns after a heading line' { & $write 'r7.md' "Frame`n$wrap16`n" } 'add r7' $true
        $needleB64 = [Convert]::ToBase64String($script:Utf8.GetBytes("QQ$needle"))
        $wrap12 = ($needleB64 -split '(?<=\G.{12})(?!$)') -join "`n"
        & $add 'needle in base64 wrapped at 12 columns after a heading line' { & $write 'r8.md' "Frame`n$wrap12`n" } 'add r8' $true
        $cleanB64 = [Convert]::ToBase64String($script:Utf8.GetBytes('hello world, this is only text, and then some more text here.'))
        $wrapClean = ($cleanB64 -split '(?<=\G.{16})(?!$)') -join "`n"
        & $add 'clean base64 wrapped at 16 columns after a heading line' { & $write 'r9.md' "Frame`n$wrapClean`n" } 'add r9' $false
        & $add 'clean commit after the normalized forms'{ & $write 'p11.txt' "12 34 56 78`nnothing here`n" } 'add p11' $false

        foreach ($s in $scenarios) {
            $r = Invoke-LeakCheck -Root $tmp -Base $s.Base -Head $s.Head -SourceRoot '' -ExtraNeedle @($needle, $digits, 'zz&tail-needle-0002') `
                -ExtraPhotoRoot @() -IncludeChangedFiles $false -NoLocalSources $true 6>$null
            $got = ($r.Exit -eq 1)
            if ($got -ne $s.ExpectHit) { $failures.Add("scenario '$($s.Name)': expected hit=$($s.ExpectHit), got exit=$($r.Exit)") }
        }
        # History is published too: a needle added and removed later is still found over the whole range.
        $first = ($scenarios | Where-Object { $_.Name -eq 'plain added line' }).Base
        $removed = ($scenarios | Where-Object { $_.Name -like 'needle removed*' }).Head
        $r = Invoke-LeakCheck -Root $tmp -Base $first -Head $removed -SourceRoot '' -ExtraNeedle @($needle) -ExtraPhotoRoot @() -IncludeChangedFiles $false -NoLocalSources $true 6>$null
        if ($r.Exit -ne 1) { $failures.Add('a needle removed in a later commit must still be found in the range') }
        # No sources and no needles: must refuse (exit 2), never report clean.
        $r = Invoke-LeakCheck -Root $tmp -Base $first -Head $removed -SourceRoot (Join-Path $tmp 'missing') -ExtraNeedle @() -ExtraPhotoRoot @() -IncludeChangedFiles $false -NoLocalSources $false 6>$null
        if ($r.Exit -ne 2) { $failures.Add('missing source data must give exit 2 (cannot verify)') }
        $r = Invoke-LeakCheck -Root $tmp -Base $first -Head $removed -SourceRoot '' -ExtraNeedle @() -ExtraPhotoRoot @() -IncludeChangedFiles $false -NoLocalSources $true 6>$null
        if ($r.Exit -ne 2) { $failures.Add('no needles must give exit 2 (cannot verify)') }

        # M-1 (a): a serial from USB / WPD is also tried without its leading zeros.
        $set = New-NeedleSet
        $null = Add-DeviceNameNeedles $set 'usb#vid_04b0&pid_0000#0012345#{00000000-0000-0000-0000-000000000000}'
        $variants = New-Variants $set
        $hits = [System.Collections.Generic.List[object]]::new()
        $targets = [System.Collections.Generic.List[object]]::new()
        Add-Target $targets 'serial without zeros' 'serial 12345 shown in the camera menu' $null 'added-line'
        Find-NeedleHits $variants $targets $hits
        if ($hits.Count -eq 0) { $failures.Add('a device serial must also be found without its leading zeros') }
        if ((Get-BodySerialCount $set) -lt 2) { $failures.Add('a USB / WPD serial must give the padded and the unpadded needle') }
        $set = New-NeedleSet
        Add-SerialNeedle $set '000098765' 'usb-serial'
        if (@($set.List | Where-Object { $_.Value -eq '98765' }).Count -ne 1) { $failures.Add('USB instance serials must also be tried without leading zeros') }

        # M-1 (b)(c): no body serial = exit 2; the export folder named in a record is read (read only).
        $source = Join-Path $tmp 'source'
        $exports = Join-Path $tmp 'zz-export-dir-0001'
        New-Item -ItemType Directory -Force -Path $source, $exports | Out-Null
        $record = Join-Path $source 'record.json'
        $exportJson = '"exportDirectory": "' + ($exports -replace '\\', '\\') + '"'
        $recordId = '0f1e2d3c4b5a69788796a5b4c3d2e1f0'   # invented
        $clean = ($scenarios | Where-Object { $_.Name -eq 'clean commit' })
        $localArgs = @{ Root = $tmp; Base = $clean.Base; Head = $clean.Head; SourceRoot = $source; ExtraNeedle = @(); ExtraPhotoRoot = @()
                        IncludeChangedFiles = $false; NoLocalSources = $false; SelfTestIsolation = $true }

        # L1 (a): records without any ID of their own (hex32 / hex64 / run ID / approval reference) cannot be verified.
        # (The export folder path holds the random name of this temporary folder, so the photograph sits in the source folder here.)
        [IO.File]::WriteAllText($record, '{"note": "x"}', $script:Utf8)
        $photo = Join-Path $source 'own.jpg'
        [IO.File]::WriteAllBytes($photo, (New-SelfTestJpeg '0012345'))
        $cap = Invoke-CheckCaptured $localArgs
        if ($cap.Result.Exit -ne 2 -or $cap.Text -notmatch 'No ID \(') { $failures.Add("records without any ID must give exit 2 even with a photographed serial (got $($cap.Result.Exit))") }
        Remove-Item -LiteralPath $photo

        # L1 (b): another source folder than the default is said in one line, without its path.
        if ($cap.Text -notmatch 'note: -SourceRoot is not the default' -or $cap.Text.Contains($tmp)) { $failures.Add('a source folder other than the default must be noted in one line without its path') }
        if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA) -and $null -ne (Get-SourceRootNote (Join-Path $env:LOCALAPPDATA 'A0CameraStitcher'))) { $failures.Add('the default source folder must not be noted') }

        [IO.File]::WriteAllText($record, ('{' + $exportJson + ', "transactionId": "' + $recordId + '", "cameraAlias": "CAM-A"}'), $script:Utf8)
        $cap = Invoke-CheckCaptured $localArgs
        if ($cap.Result.Exit -ne 2 -or $cap.Text -notmatch 'No body serial') { $failures.Add("a source folder without a body serial number must give exit 2 (got $($cap.Result.Exit))") }
        $photo = Join-Path $exports 'x.jpg'
        [IO.File]::WriteAllBytes($photo, (New-SelfTestJpeg '0012345'))
        $photoHash = (Get-FileHash -LiteralPath $photo -Algorithm SHA256).Hash
        $local = Get-LocalNeedles $source @() $true $true
        if ($local.ExportDirs -ne 1 -or $local.ExportPhotos -ne 1) { $failures.Add('the export folder named in a record must be read for photographs') }
        $serials = @($local.Set.List | Where-Object { $_.Category -eq 'exif-serial' } | ForEach-Object { $_.Value })
        if (($serials -notcontains '0012345') -or ($serials -notcontains '12345')) { $failures.Add('the EXIF serial must be read, with and without leading zeros') }
        $cap = Invoke-CheckCaptured $localArgs
        if ($cap.Result.Exit -ne 0) { $failures.Add("a source folder with a photographed serial and a clean range must give exit 0 (got $($cap.Result.Exit))") }
        if ((Get-FileHash -LiteralPath $photo -Algorithm SHA256).Hash -ne $photoHash) { $failures.Add('the export folder must be read only') }

        # L1 (c): records that name two camera bodies need two distinct body serials.
        [IO.File]::WriteAllText($record, ('{' + $exportJson + ', "transactionId": "' + $recordId + '", "legs": ["CAM-A", "CAM-B"]}'), $script:Utf8)
        $cap = Invoke-CheckCaptured $localArgs
        if ($cap.Result.Exit -ne 2 -or $cap.Text -notmatch 'name 2 camera bodies') { $failures.Add("two camera bodies in the records and one body serial must give exit 2 (got $($cap.Result.Exit))") }
        $photo2 = Join-Path $exports 'y.jpg'
        [IO.File]::WriteAllBytes($photo2, (New-SelfTestJpeg '0054321'))
        $cap = Invoke-CheckCaptured $localArgs
        if ($cap.Result.Exit -ne 0) { $failures.Add("two camera bodies in the records and two body serials must give exit 0 (got $($cap.Result.Exit))") }
        Remove-Item -LiteralPath $photo2

        # M-1 (d): -NoLocalSources outside the self test is refused.
        $out = & { Invoke-Entry -Base $first -Head $removed -RepositoryRoot $tmp -SourceRoot '' -ExtraNeedle @($needle) -ExtraPhotoRoot @() `
                -IncludeChangedFiles $false -NoLocalSources $true -InSelfTest $false } 6>&1
        if (@($out)[-1] -ne 2) { $failures.Add('-NoLocalSources outside the self test must give exit 2') }

        # L-1: no path and no exception message in the output.
        $marker = 'zz-no-such-folder-marker'
        $text = (& { Invoke-Entry -Base $first -Head $removed -RepositoryRoot (Join-Path $tmp $marker) -SourceRoot '' -ExtraNeedle @($needle) `
                    -ExtraPhotoRoot @() -IncludeChangedFiles $false -NoLocalSources $true -InSelfTest $true } 6>&1 | Out-String)
        if ($text.Contains($marker) -or $text.Contains($tmp) -or $text -notmatch 'CANNOT VERIFY') { $failures.Add('a run in a missing folder must stop with a fixed sentence and no path') }
        $text = (& { Invoke-Entry -Base 'zz-no-such-ref-marker' -Head 'HEAD' -RepositoryRoot $tmp -SourceRoot '' -ExtraNeedle @($needle) `
                    -ExtraPhotoRoot @() -IncludeChangedFiles $false -NoLocalSources $true -InSelfTest $true } 6>&1 | Out-String)
        if ($text -match 'unknown revision|ambiguous argument|fatal' -or $text.Contains($tmp)) { $failures.Add('a git failure must not put git output or a path into the output') }
        $exitFailing = @(& { Invoke-Entry -Base 'zz-no-such-ref-marker' -Head 'HEAD' -RepositoryRoot $tmp -SourceRoot '' -ExtraNeedle @($needle) `
                    -ExtraPhotoRoot @() -IncludeChangedFiles $false -NoLocalSources $true -InSelfTest $true } 6>$null)[-1]
        if ($exitFailing -ne 2) { $failures.Add('a git failure must give exit 2') }
        $text = (& { Invoke-Entry -Base $first -Head $removed -RepositoryRoot $tmp -SourceRoot '' -ExtraNeedle @($needle) -ExtraPhotoRoot @() `
                    -IncludeChangedFiles $false -NoLocalSources $true -InSelfTest $true } 6>&1 | Out-String)
        if ($text.Contains($tmp)) { $failures.Add('the output of a normal run must not hold an absolute path') }

        # L-3: a short plain word is skipped only as the account name or a generic word; any other
        # one is compared as a whole word, case-sensitively.
        $set = New-NeedleSet
        Add-Needle $set 'Zorb' 'regowner' -AccountName 'Quux'
        Add-Needle $set 'user' 'regowner' -AccountName 'Quux'
        Add-Needle $set 'Quux' 'username' -AccountName 'quux'
        if (@($set.List).Count -ne 1) { $failures.Add('only the short word that is neither the account name nor a generic word may be kept') }
        if ($set.Skipped.Count -ne 2) { $failures.Add('the account name and the generic word must be reported as skipped') }
        $variants = New-Variants $set
        foreach ($case in @(
                @{ Text = 'owner is Zorb here'; Hit = $true }, @{ Text = '(Zorb)'; Hit = $true }, @{ Text = 'zorb in lower case'; Hit = $false },
                @{ Text = 'Zorbing'; Hit = $false }, @{ Text = 'azorb'; Hit = $false }, @{ Text = 'ZORB'; Hit = $false })) {
            $hits = [System.Collections.Generic.List[object]]::new()
            $targets = [System.Collections.Generic.List[object]]::new()
            Add-Target $targets 'short word' $case.Text $null 'added-line'
            Find-NeedleHits $variants $targets $hits
            if (($hits.Count -gt 0) -ne $case.Hit) { $failures.Add("a short word needle: expected hit=$($case.Hit) for one of the sample texts") }
        }

        # L2, L4 and L7 run in repositories of their own: the folder above holds the files of the other tests.
        $tmp2 = "$tmp-r2"
        New-SelfTestRepo $tmp2
        [IO.File]::WriteAllText((Join-Path $tmp2 'seed.txt'), "seed`n", $script:Utf8)
        $seed2 = Add-SelfTestCommit $tmp2 'seed'

        # L2: a refs/replace entry must not change what the check reads (the push sends the real objects).
        [IO.File]::WriteAllText((Join-Path $tmp2 'zr.txt'), "replaced $needle`n", $script:Utf8)
        $replaceHead = Add-SelfTestCommit $tmp2 'add zr'
        $blob = (Invoke-SelfTestGit $tmp2 @('rev-parse', "${replaceHead}:zr.txt"))
        $cleanBlob = ('clean' | & git -C $tmp2 hash-object -w --stdin)
        $null = Invoke-SelfTestGit $tmp2 @('replace', '-f', $blob, $cleanBlob)
        try {
            if (((Invoke-SelfTestGit $tmp2 @('show', "${replaceHead}:zr.txt")) -join '').Contains($needle)) { $failures.Add('self test setup: the replacement must hide the needle from a plain git show') }
            $r = Invoke-LeakCheck -Root $tmp2 -Base $seed2 -Head $replaceHead -SourceRoot '' -ExtraNeedle @($needle) -ExtraPhotoRoot @() `
                -IncludeChangedFiles $false -NoLocalSources $true 6>$null
            if ($r.Exit -ne 1) { $failures.Add("a replaced object must not hide a needle (got exit $($r.Exit))") }
        }
        finally { $null = Invoke-SelfTestGit $tmp2 @('replace', '-d', $blob) }

        # L4: the path printed for a hit hides the part of it that is a needle.
        [IO.File]::WriteAllText((Join-Path $tmp2 "n-$needle.md"), "id $digits only`n", $script:Utf8)
        $hideHead = Add-SelfTestCommit $tmp2 'add hide'
        $cap = Invoke-CheckCaptured @{ Root = $tmp2; Base = $replaceHead; Head = $hideHead; SourceRoot = ''; ExtraNeedle = @($needle, $digits); ExtraPhotoRoot = @()
                                       IncludeChangedFiles = $false; NoLocalSources = $true }
        if ($cap.Result.Exit -ne 1) { $failures.Add("a needle in a file name and in its content must give exit 1 (got $($cap.Result.Exit))") }
        if ($cap.Text.Contains($needle) -or $cap.Text.Contains($digits)) { $failures.Add('a path printed for a hit must not show the needle') }
        if ($cap.Text -notmatch 'n-\*\*\*\.md') { $failures.Add('the part of a printed path that is a needle must be replaced by asterisks') }

        # L7: -IncludeChangedFiles reads what the push adds (Base...Head), not what only Base changed.
        $tmp7 = "$tmp-l7"
        New-SelfTestRepo $tmp7
        $git7 = { Invoke-SelfTestGit $tmp7 $args | Out-Null }
        [IO.File]::WriteAllText((Join-Path $tmp7 'q.txt'), "line one`nold value $needle`n", $script:Utf8)
        & $git7 add -A
        & $git7 commit -q -m fork
        $fork = (Invoke-SelfTestGit $tmp7 @('rev-parse', 'HEAD'))
        & $git7 checkout -q -b zz-base
        [IO.File]::WriteAllText((Join-Path $tmp7 'q.txt'), "line one`nclean`n", $script:Utf8)
        & $git7 add -A
        & $git7 commit -q -m 'base changes q'
        $base7 = (Invoke-SelfTestGit $tmp7 @('rev-parse', 'HEAD'))
        & $git7 checkout -q -b zz-head $fork
        [IO.File]::WriteAllText((Join-Path $tmp7 'r.txt'), "clean`n", $script:Utf8)
        & $git7 add -A
        & $git7 commit -q -m 'head adds r'
        $head7 = (Invoke-SelfTestGit $tmp7 @('rev-parse', 'HEAD'))
        $r = Invoke-LeakCheck -Root $tmp7 -Base $base7 -Head $head7 -SourceRoot '' -ExtraNeedle @($needle) -ExtraPhotoRoot @() `
            -IncludeChangedFiles $true -NoLocalSources $true 6>$null
        if ($r.Exit -ne 0) { $failures.Add("-IncludeChangedFiles must not read a file only Base changed (got exit $($r.Exit))") }
        [IO.File]::WriteAllText((Join-Path $tmp7 'r.txt'), "uncommitted $needle`n", $script:Utf8)
        $r = Invoke-LeakCheck -Root $tmp7 -Base $base7 -Head $head7 -SourceRoot '' -ExtraNeedle @($needle) -ExtraPhotoRoot @() `
            -IncludeChangedFiles $true -NoLocalSources $true 6>$null
        if ($r.Exit -ne 1) { $failures.Add("-IncludeChangedFiles must still read a file the push adds (got exit $($r.Exit))") }
    }
    finally {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
        foreach ($extra in "$tmp-r2", "$tmp-l7") {
            if (Test-Path -LiteralPath $extra) { Remove-Item -LiteralPath $extra -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }
    if ($failures.Count -eq 0) { Write-Host 'SELFTEST PASS (invented values only)'; return 0 }
    foreach ($f in $failures) { Write-Host "SELFTEST FAIL: $f" }
    return 1
}

# ------------------------------------------------------------------ entry point

$previousOutputEncoding = [Console]::OutputEncoding
try {
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    $OutputEncoding = [Text.UTF8Encoding]::new($false)
    if ($SelfTest) { exit (Invoke-SelfTest) }
    $exitCode = Invoke-Entry -Base $Base -Head $Head -RepositoryRoot $RepositoryRoot -SourceRoot $SourceRoot -ExtraNeedle $ExtraNeedle `
        -ExtraPhotoRoot $ExtraPhotoRoot -IncludeChangedFiles $IncludeChangedFiles.IsPresent -NoLocalSources $NoLocalSources.IsPresent -InSelfTest $false
    exit $exitCode
}
catch {
    # The self test holds invented values only, so it may say what went wrong; a real run may not.
    if ($SelfTest) { Write-Host "SELFTEST ERROR: $($_.Exception.Message) $($_.ScriptStackTrace)" }
    Write-Host "CANNOT VERIFY: the check stopped on an error of kind $($_.Exception.GetType().Name). Do not push."
    exit 2
}
finally {
    [Console]::OutputEncoding = $previousOutputEncoding
}
