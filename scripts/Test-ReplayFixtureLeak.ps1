<#
.SYNOPSIS
  Local check before pushing: compares what the push would publish against the real identifiers
  on this PC. Issue #241.

.DESCRIPTION
  The replay fixtures (tests/fixtures/hardware-replay) come from real operator sessions and the
  repository is public. The shape checks in the C# tests (HardwareReplayAnonymizationRules.Scan)
  cannot find a value whose shape is unremarkable, such as a 7-character body serial or an
  approval GUID. This script closes that gap.

  Real values are read at run time and kept in memory only:
    - the records under %LOCALAPPDATA%\A0CameraStitcher (IDs, hashes, run IDs, sizes, dates, paths)
    - EXIF of the original photographs found there (owner, serials, dates, unique ID, maker note)
    - the registry (Nikon USB / WPD device keys, registered owner)
    - the environment (PC name, user profile paths)
  Nothing is written to disk and no value or hash of one is stored in the repository. Output has
  only labels (category#number) and locations (commit, file, line). Values are never printed.

  What is compared (the content a push publishes): for every commit in <Base>..<Head>, the commit
  message, the added lines of the diff, the decoded text of hex and base64 strings found in added
  lines (including terminalResultHex), and the decoded content of every added or changed .b64
  file. Each needle is also tried as dashed GUID, as hex of its UTF-8 bytes and as base64 in the
  three alignments. Binary files in the range are reported as hits. -IncludeChangedFiles also
  compares the whole HEAD content of every file the range touches.

  Exit codes: 0 = no hits, 1 = hits (judge each: a hit may be a false positive such as a real date
  that also appears in prose, but a real value must not be pushed), 2 = could not verify
  (no local source data, no needles, or an error). Treat 2 as "do not push".

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

.PARAMETER SelfTest
  Runs the check against a throw-away repository with invented values and reports PASS or FAIL.
  Needs no local source data.

.EXAMPLE
  pwsh -NoProfile -File scripts/Test-ReplayFixtureLeak.ps1 -Base origin/main
#>
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
    param($Set, [string]$Value, [string]$Category)
    if ([string]::IsNullOrWhiteSpace($Value)) { return }
    $Value = $Value.Trim()
    if ($Value.Length -lt 4) { return }
    # A short plain word (an account or owner name such as a bare first name or "user") matches
    # ordinary prose everywhere; the profile path forms below cover the real exposure. Say so.
    if ($Category -in 'regowner', 'username', 'pcname' -and $Value.Length -lt 5 -and $Value -match '^[A-Za-z]+$') {
        if (-not $Set.Skipped.ContainsKey($Category)) { $Set.Skipped[$Category] = 0 }
        $Set.Skipped[$Category]++
        return
    }
    if (-not $Set.Seen.Add($Value)) { return }
    if (-not $Set.Count.ContainsKey($Category)) { $Set.Count[$Category] = 0 }
    $Set.Count[$Category]++
    $Set.List.Add([pscustomobject]@{ Value = $Value; Category = $Category; Label = "$Category#$($Set.Count[$Category])" })
}

$script:TextExtensions = @('.json', '.jsonl', '.txt', '.log', '.md', '.csv', '.yaml', '.yml', '.xml', '.ini', '')

function Add-NeedlesFromText {
    param($Set, [string]$Text)
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
            0xA431 { Add-Needle $Set (Get-ExifAscii $buf $e) 'exif-serial' }
            0xA435 { Add-Needle $Set (Get-ExifAscii $buf $e) 'exif-serial' }
            0x9286 {
                if ($e.Total -gt 8) { Add-Needle $Set ($script:Latin1.GetString($buf, $e.Off + 8, $e.Total - 8).Replace([string][char]0, '').Trim()) 'exif-owner' }
            }
            0x927C {
                if ($script:Latin1.GetString($buf, $e.Off, 5) -eq 'Nikon') {
                    $mb = $e.Off + 10; $mbe = ($script:Latin1.GetString($buf, $mb, 2) -eq 'MM')
                    foreach ($me in (Get-Ifd $buf $mb (Get-U32 $buf ($mb + 4) $mbe) $mbe)) {
                        if ($me.Tag -eq 0x001D -and $me.Type -eq 2) {
                            $v = Get-ExifAscii $buf $me
                            Add-Needle $Set $v 'exif-serial'
                            Add-Needle $Set $v.TrimStart('0') 'exif-serial'
                        }
                        elseif ($me.Tag -eq 0x00A7 -and $me.Type -eq 4) { Add-Needle $Set ([string](Get-U32 $buf $me.Off $mbe)) 'exif-shuttercount' }
                        elseif ($me.Type -eq 2) { $v = Get-ExifAscii $buf $me; if ($v -match '\d{4,}') { Add-Needle $Set $v 'exif-mn-numeric-ascii' } }
                    }
                }
            }
        }
    }
}

function Get-LocalNeedles {
    param([string]$SourceRoot, [string[]]$ExtraPhotoRoot)
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
    foreach ($t in $texts) { Add-NeedlesFromText $set $t }

    # Original photographs: hash, size, EXIF.
    $photoFiles = @($sourceFiles | Where-Object { $_.Extension -in '.jpg', '.jpeg' })
    foreach ($root in $ExtraPhotoRoot) {
        if (Test-Path -LiteralPath $root -PathType Container) {
            $photoFiles += @(Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.Extension -in '.jpg', '.jpeg' })
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

    # USB / WPD device identity from the registry (read only).
    $usbCount = 0; $wpdCount = 0
    foreach ($k in Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\USB' -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -match '^VID_04B0' }) {
        Add-Needle $set $k.PSChildName 'nikon-vidpid'
        foreach ($c in Get-ChildItem $k.PSPath -ErrorAction SilentlyContinue) {
            $usbCount++
            Add-Needle $set $c.PSChildName 'usb-serial'
            Add-Needle $set ($k.PSChildName + '\' + $c.PSChildName) 'usb-instanceid'
            Add-Needle $set ($k.PSChildName + '#' + $c.PSChildName) 'usb-instanceid'
        }
    }
    foreach ($root in @('HKLM:\SOFTWARE\Microsoft\Windows Portable Devices\Devices', 'HKLM:\SYSTEM\CurrentControlSet\Enum\SWD\WPDBUSENUM')) {
        foreach ($c in Get-ChildItem $root -ErrorAction SilentlyContinue) {
            $name = $c.PSChildName
            if ($name -match '(?i)VID_04B0&PID_[0-9A-F]{4}[#\\]([^#\\]+)') { $wpdCount++; Add-Needle $set $Matches[1] 'usb-serial'; Add-Needle $set $name 'wpd-devicekey' }
        }
    }
    [pscustomobject]@{ Set = $set; SourceFiles = $sourceFiles.Count; Photos = $photoFiles.Count; UsbInstances = $usbCount; WpdKeys = $wpdCount }
}

# ------------------------------------------------------------------ variants

function New-Variants {
    param($Set)
    $variants = [System.Collections.Generic.List[object]]::new()
    foreach ($n in $Set.List) {
        $v = $n.Value
        $numeric = ($v -match '^\d+$')
        $variants.Add([pscustomobject]@{ Needle = $n; Text = $v; Kind = 'plain'; Compare = [StringComparison]::OrdinalIgnoreCase; DigitBoundary = $numeric })
        if ($n.Category -eq 'hex32') {
            $d = $v.Substring(0, 8) + '-' + $v.Substring(8, 4) + '-' + $v.Substring(12, 4) + '-' + $v.Substring(16, 4) + '-' + $v.Substring(20)
            $variants.Add([pscustomobject]@{ Needle = $n; Text = $d; Kind = 'guid-dashed'; Compare = [StringComparison]::OrdinalIgnoreCase; DigitBoundary = $false })
        }
        if ($v.Length -ge 6) {
            $bytes = $script:Utf8.GetBytes($v)
            $variants.Add([pscustomobject]@{ Needle = $n; Text = [Convert]::ToHexString($bytes); Kind = 'hex-encoded'; Compare = [StringComparison]::OrdinalIgnoreCase; DigitBoundary = $false })
            for ($k = 0; $k -lt 3; $k++) {
                $all = [byte[]](@([byte[]]::new($k) | ForEach-Object { [byte]0x41 }) + $bytes)
                $enc = [Convert]::ToBase64String($all)
                $start = if ($k -eq 0) { 0 } else { 4 }
                $full = [int]([math]::Floor(($k + $bytes.Length) / 3) * 4)
                if ($full - $start -ge 8) {
                    $variants.Add([pscustomobject]@{ Needle = $n; Text = $enc.Substring($start, $full - $start); Kind = "b64-align$k"; Compare = [StringComparison]::Ordinal; DigitBoundary = $false })
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
    if ($Target.Lines) { return $Target.Lines[[math]::Min($line, $Target.Lines.Length) - 1] }
    return $line
}

function Add-DecodedTargets {
    # Hex and base64 strings found in published text are decoded and compared as text too.
    param($Targets, [string]$Name, [string]$Text)
    foreach ($m in [regex]::Matches($Text, '(?<![0-9a-fA-F])(?:[0-9a-fA-F]{2}){8,}(?![0-9a-fA-F])')) {
        $decoded = $script:Utf8.GetString([Convert]::FromHexString($m.Value))
        Add-Target $Targets "hex-decoded $Name" $decoded $null 'decoded-hex'
        Add-Target $Targets "hex-decoded-latin1 $Name" ($script:Latin1.GetString([Convert]::FromHexString($m.Value))) $null 'decoded-hex'
    }
    foreach ($m in [regex]::Matches($Text, '(?<![A-Za-z0-9+/])[A-Za-z0-9+/]{16,}={0,2}(?![A-Za-z0-9+/=])')) {
        $s = $m.Value.TrimEnd('=')
        $s = $s.PadRight($s.Length + ((4 - $s.Length % 4) % 4), '=')
        try { $bytes = [Convert]::FromBase64String($s) } catch { continue }
        Add-Target $Targets "b64-decoded $Name" ($script:Latin1.GetString($bytes)) $null 'decoded-b64'
        Add-Target $Targets "b64-decoded-utf16le $Name" ([Text.Encoding]::Unicode.GetString($bytes)) $null 'decoded-b64'
    }
}

function Invoke-Git {
    param([string]$Root, [string[]]$GitArgs)
    $out = & git -C $Root -c core.quotepath=off @GitArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed: $($out -join ' ')" }
    return $out
}

function Get-PublishTargets {
    param([string]$Root, [string]$Base, [string]$Head, [bool]$IncludeChangedFiles)
    $targets = [System.Collections.Generic.List[object]]::new()
    $structural = [System.Collections.Generic.List[object]]::new()
    $commits = @(Invoke-Git $Root @('rev-list', '--reverse', "$Base..$Head"))
    Write-Host "commits in range $Base..$Head : $($commits.Count)"
    $merges = @(Invoke-Git $Root @('rev-list', '--merges', "$Base..$Head"))
    foreach ($c in $merges) { $structural.Add([pscustomobject]@{ Scope = 'merge-commit'; Target = $c.Substring(0, 7); Line = 0; Label = 'merge' }) }

    foreach ($c in $commits) {
        $short = $c.Substring(0, 7)
        $message = (Invoke-Git $Root @('log', '-1', '--format=%B', $c)) -join "`n"
        Add-Target $targets "commit-message $short" $message $null 'commit-message'
        Add-DecodedTargets $targets "commit-message $short" $message

        foreach ($line in (Invoke-Git $Root @('diff-tree', '--no-commit-id', '--numstat', '-r', $c))) {
            if ($line -match "^-`t-`t(.+)$") { $structural.Add([pscustomobject]@{ Scope = 'binary-file'; Target = "$short $($Matches[1])"; Line = 0; Label = 'binary' }) }
        }

        $diff = (Invoke-Git $Root @('show', '--no-color', '--format=', '--unified=0', $c)) -join "`n"
        $file = $null; $newLine = 0; $inHunk = $false
        $buf = [System.Collections.Generic.List[string]]::new(); $nums = [System.Collections.Generic.List[int]]::new()
        $flush = {
            if ($file -and $buf.Count -gt 0) {
                $text = $buf -join "`n"
                Add-Target $targets "added $short $file" $text $nums.ToArray() 'added-line'
                Add-DecodedTargets $targets "$short $file" $text
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

        # .b64 files are multi-line: decode the whole file as of this commit.
        foreach ($p in @(Invoke-Git $Root @('diff-tree', '--no-commit-id', '--name-only', '-r', '--diff-filter=AM', $c))) {
            if ($p -notlike '*.b64') { continue }
            $content = (Invoke-Git $Root @('show', "${c}:$p")) -join ''
            try { $bytes = [Convert]::FromBase64String(($content -replace '\s', '')) } catch {
                $structural.Add([pscustomobject]@{ Scope = 'unreadable-b64'; Target = "$short $p"; Line = 0; Label = 'b64' }); continue
            }
            Add-Target $targets "b64-file-latin1 $short $p" ($script:Latin1.GetString($bytes)) $null 'decoded-b64'
            Add-Target $targets "b64-file-utf16le $short $p" ([Text.Encoding]::Unicode.GetString($bytes)) $null 'decoded-b64'
            if ($bytes.Length -gt 1) { Add-Target $targets "b64-file-utf16le+1 $short $p" ([Text.Encoding]::Unicode.GetString($bytes, 1, $bytes.Length - 1)) $null 'decoded-b64' }
        }
    }

    if ($IncludeChangedFiles -and $commits.Count -gt 0) {
        foreach ($p in @(Invoke-Git $Root @('diff', '--name-only', '--diff-filter=AM', "$Base..$Head"))) {
            $full = Join-Path $Root $p
            if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }
            $bytes = [IO.File]::ReadAllBytes($full)
            if ($bytes.Length -gt 3MB -or [Array]::IndexOf($bytes, [byte]0) -ge 0) { continue }
            Add-Target $targets "tree $p" ($script:Utf8.GetString($bytes)) $null 'changed-file'
        }
    }
    return [pscustomobject]@{ Targets = $targets; Structural = $structural; CommitCount = $commits.Count }
}

function Test-Boundary([string]$Text, [int]$Index, [int]$Length) {
    if ($Index -gt 0 -and [char]::IsDigit($Text[$Index - 1])) { return $false }
    $end = $Index + $Length
    if ($end -lt $Text.Length -and [char]::IsDigit($Text[$end])) { return $false }
    return $true
}

# ------------------------------------------------------------------ main check

function Invoke-LeakCheck {
    param(
        [string]$Root, [string]$Base, [string]$Head, [string]$SourceRoot, [string[]]$ExtraNeedle,
        [string[]]$ExtraPhotoRoot, [bool]$IncludeChangedFiles, [bool]$NoLocalSources)

    $set = New-NeedleSet
    $cannotVerify = $null
    if (-not $NoLocalSources) {
        if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
            $cannotVerify = "The local source data folder does not exist: nothing real to compare against."
        }
        else {
            $local = Get-LocalNeedles $SourceRoot $ExtraPhotoRoot
            $set = $local.Set
            Write-Host "local sources: record files=$($local.SourceFiles), photographs=$($local.Photos), nikon usb instances=$($local.UsbInstances), wpd keys=$($local.WpdKeys)"
            if ($local.SourceFiles -eq 0) { $cannotVerify = 'The local source data folder holds no files.' }
        }
    }
    foreach ($v in $ExtraNeedle) { Add-Needle $set $v 'extra' }

    Write-Host '=== needle inventory (counts per category; values withheld) ==='
    $set.List | Group-Object Category | Sort-Object Name | ForEach-Object { Write-Host ('{0,-26} {1}' -f $_.Name, $_.Count) }
    foreach ($k in $set.Skipped.Keys) { Write-Host ('skipped as too generic (short plain word): {0} x{1}' -f $k, $set.Skipped[$k]) }
    if ($set.List.Count -eq 0 -and -not $cannotVerify) { $cannotVerify = 'No needles could be built.' }
    if ($cannotVerify) {
        Write-Host "CANNOT VERIFY: $cannotVerify Do not push."
        return [pscustomobject]@{ Exit = 2; Hits = @(); Needles = $set.List.Count }
    }

    $variants = New-Variants $set
    Write-Host "needles=$($set.List.Count) variants=$($variants.Count)"
    $published = Get-PublishTargets $Root $Base $Head $IncludeChangedFiles
    Write-Host "targets=$($published.Targets.Count)"

    $seen = [System.Collections.Generic.HashSet[string]]::new()
    $hits = [System.Collections.Generic.List[object]]::new()
    foreach ($s in $published.Structural) { $hits.Add($s) }
    foreach ($t in $published.Targets) {
        foreach ($v in $variants) {
            $idx = $t.Text.IndexOf($v.Text, $v.Compare)
            while ($idx -ge 0) {
                if (-not $v.DigitBoundary -or (Test-Boundary $t.Text $idx $v.Text.Length)) {
                    $line = Get-LineOf $t $idx
                    if ($seen.Add("$($t.Scope)|$($t.Name)|$line|$($v.Needle.Label)|$($v.Kind)")) {
                        $hits.Add([pscustomobject]@{ Scope = $t.Scope; Target = $t.Name; Line = $line; Label = $v.Needle.Label; Kind = $v.Kind })
                    }
                }
                $idx = $t.Text.IndexOf($v.Text, $idx + 1, $v.Compare)
            }
        }
    }

    Write-Host '=== HITS (labels and locations only; values withheld) ==='
    foreach ($g in $hits | Group-Object Scope) {
        Write-Host "## scope=$($g.Name) hits=$($g.Count)"
        foreach ($h in ($g.Group | Sort-Object Target, Line | Select-Object -First 80)) {
            Write-Host ('  {0} :{1}  {2} ({3})' -f $h.Target, $h.Line, $h.Label, $(if ($h.PSObject.Properties['Kind']) { $h.Kind } else { '-' }))
        }
    }
    if ($hits.Count -eq 0) { Write-Host 'NO HITS in the content this push would publish.' }
    Write-Host "=== SUMMARY: hits in content this push would publish = $($hits.Count)"
    return [pscustomobject]@{ Exit = $(if ($hits.Count -eq 0) { 0 } else { 1 }); Hits = $hits; Needles = $set.List.Count }
}

# ------------------------------------------------------------------ self test (invented values only)

function Invoke-SelfTest {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("a0-leak-selftest-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    $needle = 'zz-selftest-needle-0001'
    $digits = '7654321'
    $failures = [System.Collections.Generic.List[string]]::new()
    try {
        $git = { $o = & git -C $tmp @args 2>&1; if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' '): $($o -join ' ')" } }
        & $git init -q
        & $git config user.name selftest
        & $git config user.email selftest@example.invalid
        & $git config core.autocrlf false
        & $git config commit.gpgsign false
        function Commit([string]$Message) { & $git add -A; & $git commit -q -m $Message; return (& git -C $tmp rev-parse HEAD) }
        $write = { param([string]$Rel, [string]$Text) $p = Join-Path $tmp $Rel; New-Item -ItemType Directory -Force -Path (Split-Path $p) | Out-Null; [IO.File]::WriteAllText($p, $Text, $script:Utf8) }
        $scenarios = [System.Collections.Generic.List[object]]::new()
        $prev = $null
        $add = {
            param([string]$Name, [scriptblock]$Setup, [string]$Message, [bool]$ExpectHit)
            & $Setup
            $c = Commit $Message
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

        foreach ($s in $scenarios) {
            $r = Invoke-LeakCheck -Root $tmp -Base $s.Base -Head $s.Head -SourceRoot '' -ExtraNeedle @($needle, $digits) `
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
    }
    finally {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
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

    if ([string]::IsNullOrWhiteSpace($Base)) {
        & git -C $RepositoryRoot rev-parse --verify --quiet '@{u}' *> $null
        $Base = if ($LASTEXITCODE -eq 0) { '@{u}' } else { 'origin/main' }
    }
    Write-Host "repository=$RepositoryRoot base=$Base head=$Head"
    $result = Invoke-LeakCheck -Root $RepositoryRoot -Base $Base -Head $Head -SourceRoot $SourceRoot -ExtraNeedle $ExtraNeedle `
        -ExtraPhotoRoot $ExtraPhotoRoot -IncludeChangedFiles $IncludeChangedFiles.IsPresent -NoLocalSources $NoLocalSources.IsPresent
    exit $result.Exit
}
catch {
    Write-Host "CANNOT VERIFY: $($_.Exception.Message) Do not push."
    exit 2
}
finally {
    [Console]::OutputEncoding = $previousOutputEncoding
}
