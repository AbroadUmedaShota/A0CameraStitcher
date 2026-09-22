#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidatePattern('^[a-z0-9][a-z0-9-]{0,63}$')]
    [string]$CandidateName = ('software-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Windows is required.' }
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$nativeBuild = Join-Path $repository "build/local-software-native/$CandidateName"
$candidate = Join-Path $repository "build/local-software-candidates/$CandidateName"

function Assert-LocalUnredirected([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full.StartsWith('\\') -or ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($full))).DriveType -ne 'Fixed') {
        throw 'A fixed local drive is required.'
    }
    $current = $full
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            if (((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'Redirected paths are not allowed.'
            }
        }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}

function Invoke-Dotnet([string[]]$Arguments) {
    & dotnet @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'dotnet packaging failed; the incomplete candidate is retained.' }
}

function Assert-Files([string]$Directory, [string[]]$Expected) {
    if (@(Get-ChildItem -LiteralPath $Directory -Directory).Count -ne 0) { throw 'Unexpected publish subdirectory.' }
    $actual = @(Get-ChildItem -LiteralPath $Directory -File | ForEach-Object Name)
    if (@(Compare-Object ($Expected | Sort-Object) ($actual | Sort-Object)).Count -ne 0) {
        throw 'Candidate file allowlist mismatch; no completion manifest will be written.'
    }
    foreach ($file in Get-ChildItem -LiteralPath $Directory -File) { Assert-LocalUnredirected $file.FullName }
}

Assert-LocalUnredirected $repository
Assert-LocalUnredirected $candidate
Assert-LocalUnredirected $nativeBuild
if (Test-Path -LiteralPath $candidate) { throw 'Candidate already exists; overwrite and resume are prohibited.' }
if (Test-Path -LiteralPath $nativeBuild) { throw 'Candidate native build already exists; reuse is prohibited.' }
Get-Command dotnet, cmake, git -ErrorAction Stop | Out-Null

Push-Location $repository
try {
    $sourceCommit = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source commit.' }
    $sourceStatus = @(& git status --porcelain)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot observe source status.' }
    if ($sourceStatus.Count -ne 0) { throw 'A clean source worktree is required before building a candidate.' }
    & git diff --quiet HEAD -- .
    if ($LASTEXITCODE -ne 0) { throw 'Tracked source changes must be committed before building a candidate.' }
    $scriptHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    $null = New-Item -ItemType Directory -Path $candidate # Atomic refusal of an existing leaf, no -Force.
    $app = Join-Path $candidate 'app'
    $cli = Join-Path $app 'review-cli'
    $start = @{ schemaVersion = 1; status = 'building'; sourceCommit = $sourceCommit; sourceDirty = $sourceStatus.Count -ne 0;
        sourceStatus = $sourceStatus; recipeSha256 = $scriptHash; startedAtUtc = [DateTimeOffset]::UtcNow.ToString('O') }
    [IO.File]::WriteAllText((Join-Path $candidate 'build-start.json'), ($start | ConvertTo-Json -Depth 5))

    $null = New-Item -ItemType Directory -Path $nativeBuild
    & cmake -S $repository -B $nativeBuild -G 'Visual Studio 17 2022' -A x64 '-DNIKON_D810_SDK_ROOT:PATH='
    if ($LASTEXITCODE -ne 0) { throw 'Fresh SDK-free native configuration failed.' }
    $cacheLines = [IO.File]::ReadAllLines((Join-Path $nativeBuild 'CMakeCache.txt'))
    if (@($cacheLines | Where-Object { $_ -ceq 'NIKON_D810_SDK_ROOT:PATH=' }).Count -ne 1) { throw 'SDK-free configuration not confirmed.' }
    & cmake --build $nativeBuild --config Release --target A0CameraStitcher.M2Adapter A0CameraStitcher.CameraAgent A0CameraStitcher.DualCameraAgent
    if ($LASTEXITCODE -ne 0) { throw 'Fresh SDK-free native build failed.' }

    Invoke-Dotnet @('publish', 'src/m3/OperatorShell/A0CameraStitcher.M3.OperatorShell.csproj', '-c', 'Release', '--no-restore',
        '--self-contained', 'false', '-o', $app, "-p:M2AdapterBuildDirectory=$nativeBuild")
    Assert-Files $app @('A0CameraStitcher.CameraAgent.exe', 'A0CameraStitcher.DualCameraAgent.exe', 'A0CameraStitcher.M2Adapter.exe',
        'A0CameraStitcher.M3.Foundation.dll', 'A0CameraStitcher.M3.Foundation.pdb', 'A0CameraStitcher.M3.OperatorShell.exe',
        'A0CameraStitcher.M3.OperatorShell.dll', 'A0CameraStitcher.M3.OperatorShell.pdb',
        'A0CameraStitcher.M3.OperatorShell.deps.json', 'A0CameraStitcher.M3.OperatorShell.runtimeconfig.json')
    Invoke-Dotnet @('publish', 'src/m3/ReviewCli/A0CameraStitcher.M3.ReviewCli.csproj', '-c', 'Release', '--no-restore',
        '--self-contained', 'false', '-o', $cli)
    Copy-Item -LiteralPath (Join-Path $app 'A0CameraStitcher.M2Adapter.exe') -Destination $cli
    Assert-Files $cli @('A0CameraStitcher.M2Adapter.exe', 'A0CameraStitcher.M3.Foundation.dll', 'A0CameraStitcher.M3.Foundation.pdb',
        'A0CameraStitcher.M3.ReviewCli.exe', 'A0CameraStitcher.M3.ReviewCli.dll', 'A0CameraStitcher.M3.ReviewCli.pdb',
        'A0CameraStitcher.M3.ReviewCli.deps.json', 'A0CameraStitcher.M3.ReviewCli.runtimeconfig.json')
    if ((Get-FileHash (Join-Path $app 'A0CameraStitcher.M3.Foundation.dll')).Hash -ne
        (Get-FileHash (Join-Path $cli 'A0CameraStitcher.M3.Foundation.dll')).Hash) { throw 'GUI and CLI foundation builds differ.' }
    $licenses = Join-Path $candidate 'licenses'
    $null = New-Item -ItemType Directory -Path $licenses
    foreach ($name in @('NotoSansJP-OFL.txt', 'JetBrainsMono-OFL.txt')) {
        Copy-Item -LiteralPath (Join-Path $repository "src/m3/OperatorShell/Assets/Fonts/$name") -Destination $licenses
    }
    $descriptionText = & (Join-Path $cli 'A0CameraStitcher.M3.ReviewCli.exe') describe
    if ($LASTEXITCODE -ne 0) { throw 'Packaged CLI describe failed.' }
    $description = ($descriptionText -join "`n") | ConvertFrom-Json
    if ($description.appId -cne 'a0-camera-stitcher-review-cli' -or $description.version -ne 3 -or
        $description.status -cne 'ok' -or $description.data.operations -cnotcontains 'verify-review') { throw 'Packaged CLI contract mismatch.' }
    [IO.File]::WriteAllText((Join-Path $candidate 'cli-describe.json'), ($description | ConvertTo-Json -Depth 12))
    [IO.File]::WriteAllText((Join-Path $candidate 'README.txt'), @'
A0CameraStitcher - LOCAL SOFTWARE CANDIDATE ONLY
Not a hardware-ready build, approved release, or distribution authorization.
Nikon SDK/modules and captured images are not included. SDK-free camera operations remain gated.
Requires the host .NET 10 Windows Desktop runtime and native runtime dependencies; clean-PC operation is unverified.
GUI: app/A0CameraStitcher.M3.OperatorShell.exe (GUI acceptance not performed by this recipe)
CLI: app/review-cli/A0CameraStitcher.M3.ReviewCli.exe describe
Read-only verification: verify-review --product-root <absolute local product root> --result-id <GUID N> --expected-kind <Product|Simulated>
CLI gui-status observes an explicitly identified MainWindow instance; it cannot send GUI commands, accept results, operate cameras, or capture images.
Preserve this directory and its manifest. Build a new candidate rather than overwriting it.
'@)
    $files = @(Get-ChildItem -LiteralPath $candidate -File -Recurse | Sort-Object FullName | ForEach-Object {
        Assert-LocalUnredirected $_.FullName
        @{ path = [IO.Path]::GetRelativePath($candidate, $_.FullName).Replace('\', '/'); size = $_.Length;
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
    $manifest = @{ schemaVersion = 1; status = 'local-software-candidate'; sourceCommit = $sourceCommit;
        sourceDirty = $sourceStatus.Count -ne 0; recipeSha256 = $scriptHash; createdAtUtc = [DateTimeOffset]::UtcNow.ToString('O');
        hardwareAccepted = $false; guiAccepted = $false; redistributionApproved = $false; sdkIncluded = $false;
        selfContained = $false; cliContractVersion = 3; files = $files }
    $manifestPath = Join-Path $candidate 'candidate.manifest.json'
    $finalCommit = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $finalCommit -cne $sourceCommit) { throw 'Source commit changed during packaging.' }
    $finalStatus = @(& git status --porcelain)
    if ($LASTEXITCODE -ne 0 -or $finalStatus.Count -ne 0) { throw 'Source worktree changed during packaging.' }
    $manifestPartial = $manifestPath + '.partial'
    [IO.File]::WriteAllText($manifestPartial, ($manifest | ConvertTo-Json -Depth 8))
    [IO.File]::Move($manifestPartial, $manifestPath)
    Write-Output $manifestPath
}
finally { Pop-Location }
