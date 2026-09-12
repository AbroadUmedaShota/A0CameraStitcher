#requires -Version 7.4
[CmdletBinding()]
param(
    [string]$ReadinessScriptPath = (Join-Path $PSScriptRoot 'Test-Phase0Readiness.ps1'),
    # Run only the missing-identity regression against an old entry (expected RED).
    [switch]$BaselineGuardOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-That([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "ASSERT: $Message" }
}
function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Assert-Parses([string]$Path) {
    $parseErrors = $null
    $parseTokens = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$parseTokens, [ref]$parseErrors)
    Assert-That ($parseErrors.Count -eq 0) "Invalid generated PowerShell: $Path"
}

function New-FakeCli([string]$Root) {
    # Package-free, SDK-independent fixture; never build or use the actual product.
    [IO.File]::WriteAllText((Join-Path $Root 'Fake.csproj'), @'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <UseAppHost>true</UseAppHost>
    <AssemblyName>A0CameraStitcher.Phase0</AssemblyName>
  </PropertyGroup>
</Project>
'@)
    [IO.File]::WriteAllText((Join-Path $Root 'Program.cs'), @'
using System;
using System.IO;
class Program {
    static int Main(string[] args) {
        string log = Environment.GetEnvironmentVariable("A0_READINESS_FAKE_LOG");
        string call = Path.GetFullPath(Environment.ProcessPath) + "|" + string.Join(" ", args);
        File.AppendAllText(log, "start|" + call + Environment.NewLine);
        const string raw = "SYNTHETIC-RAW-DEVICE-ID-DO-NOT-PRINT";
        int result = 7;
        if (args.Length > 0 && args[0] == "preflight") {
            Console.WriteLine(raw);
            Console.Error.WriteLine(raw);
            result = Environment.GetEnvironmentVariable("A0_FAKE_PREFLIGHT_FAIL") == "1" ? 9 : 0;
        } else if (args.Length == 3 && args[0] == "inventory") {
            string transport = args[2] == "sdk" ? "SDK" : "WPD";
            string Read(string name) => Environment.GetEnvironmentVariable("A0_FAKE_" + transport + "_" + name);
            Console.WriteLine(raw);
            Console.Error.WriteLine(raw);
            Console.WriteLine("CameraCount: " + Read("COUNT"));
            Console.WriteLine("BoundCameraCount: " + Read("BOUND"));
            Console.WriteLine("UnboundCameraCount: " + Read("UNBOUND"));
            result = Read("FAIL") == "1" ? 8 : 0;
        }
        File.AppendAllText(log, "end|" + call + Environment.NewLine);
        return result;
    }
}
'@)
    $release = Join-Path $Root 'Release'
    $buildLog = Join-Path $Root 'fake-cli-build.log'
    & dotnet build (Join-Path $Root 'Fake.csproj') -c Release -o $release --nologo *> $buildLog
    Assert-That ($LASTEXITCODE -eq 0) "Fake CLI build failed: $buildLog"
    $exe = Join-Path $release 'A0CameraStitcher.Phase0.exe'
    Assert-That (Test-Path -LiteralPath $exe -PathType Leaf) 'Generated fake executable missing'
    $debug = Join-Path $Root 'Debug'
    $null = [IO.Directory]::CreateDirectory($debug)
    foreach ($file in Get-ChildItem -LiteralPath $release -File) {
        [IO.File]::Copy($file.FullName, (Join-Path $debug $file.Name))
    }
    $debugExe = Join-Path $debug 'A0CameraStitcher.Phase0.exe'
    [IO.File]::AppendAllText($debugExe, 'stale-debug-test-only')
    Assert-That ((Get-Sha256 $debugExe) -ne (Get-Sha256 $exe)) 'Debug/Release hashes must differ'
    [pscustomobject]@{ Release = $exe; Debug = $debugExe }
}

function New-ChildWrapper([string]$Root) {
    $path = Join-Path $Root 'readiness-child.ps1'
    [IO.File]::WriteAllText($path, @'
param([string]$PayloadPath, [string]$ResultPath)
$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Management -ErrorAction Stop
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop
$PSModuleAutoLoadingPreference = 'None'
$PSStyle.OutputRendering = 'PlainText'
$p = Get-Content -LiteralPath $PayloadPath -Raw | ConvertFrom-Json -AsHashtable
$global:a0ReadinessFixtureResult = [ordered]@{
    pnp = 0; cli = 0; exitCode = 1; afterMarker = $false; leaseHeld = $null
    envRestored = $false; nativePreferenceRestored = $false; pnpIsFunction = $false
    error = ''; trace = @()
}
$result = $global:a0ReadinessFixtureResult
$global:a0ReadinessFixturePayload = $p
$env:NIKON_D810_SDK_ROOT = $null
foreach ($entry in $p.FakeEnvironment.GetEnumerator()) {
    [Environment]::SetEnvironmentVariable([string]$entry.Key, [string]$entry.Value, 'Process')
}
$expectedSdkRoot = $env:NIKON_D810_SDK_ROOT
$PSNativeCommandUseErrorActionPreference = [bool]$p.NativeErrorPreference
$expectedNativePreference = $PSNativeCommandUseErrorActionPreference
$global:a0ReadinessFixtureRoot = [IO.Path]::GetFullPath($p.WorkRoot).TrimEnd('\')

function Test-FixturePath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $full.Equals($global:a0ReadinessFixtureRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($global:a0ReadinessFixtureRoot + '\', [StringComparison]::OrdinalIgnoreCase)
}
function global:Get-PnpDevice {
    [CmdletBinding()]param([switch]$PresentOnly)
    $global:a0ReadinessFixtureResult.pnp++
    if ($global:a0ReadinessFixturePayload.Parameters.ContainsKey('Phase0ExecutablePath')) {
        # Open only: never change bytes. The production read lease must deny write.
        try {
            $writeHandle = [IO.File]::Open($global:a0ReadinessFixturePayload.Parameters.Phase0ExecutablePath, 'Open', 'Write', 'ReadWrite')
            $writeHandle.Dispose()
            $global:a0ReadinessFixtureResult.leaseHeld = $false
        } catch [IO.IOException] { $global:a0ReadinessFixtureResult.leaseHeld = $true }
    }
    for ($index = 0; $index -lt [int]$env:A0_FAKE_PNP_COUNT; $index++) {
        [pscustomobject]@{ FriendlyName = 'Nikon D810'; InstanceId = 'SYNTHETIC-RAW-DEVICE-ID-DO-NOT-PRINT' }
    }
}
function global:Get-ChildItem {
    [CmdletBinding()]param([string]$LiteralPath, [switch]$Directory)
    if ($LiteralPath -eq 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC') {
        [pscustomobject]@{ Name = '14.99'; FullName = 'C:\fake-vs\14.99' }
    } elseif (Test-FixturePath $LiteralPath) {
        Microsoft.PowerShell.Management\Get-ChildItem @PSBoundParameters
    } else { throw 'Unexpected discovery outside fixture.' }
}
function global:Test-Path {
    [CmdletBinding()]param([string]$LiteralPath, [string]$PathType)
    if ($LiteralPath -in @(
        'C:\fake-vs\14.99\bin\Hostx64\x64\cl.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    )) { return $true }
    if (Test-FixturePath $LiteralPath) {
        return Microsoft.PowerShell.Management\Test-Path @PSBoundParameters
    }
    # Never discover a real SDK/default product, including the no-arguments case.
    return $false
}
function global:Get-Command {
    [CmdletBinding()]param([string]$Name)
    if ($Name -eq 'cmake') { return [pscustomobject]@{ Source = 'C:\fake-vs\cmake.exe' } }
    throw 'Unexpected command discovery.'
}
function global:Get-CimInstance { throw 'OS hardware access prohibited in fake test.' }
function global:Get-WmiObject { throw 'OS hardware access prohibited in fake test.' }
$result.pnpIsFunction = (Microsoft.PowerShell.Core\Get-Command Get-PnpDevice).CommandType -eq 'Function'
$entryParameters = $p.Parameters
try {
    # Do not dot-source: preserve the caller's scope and collect the actual exit.
    & $p.Entry @entryParameters
    $result.exitCode = $LASTEXITCODE
} catch {
    $result.error = $_.Exception.GetType().FullName + ': ' + $_.Exception.Message + ' at ' + $_.ScriptStackTrace
    $result.exitCode = 1
} finally {
    $log = $env:A0_READINESS_FAKE_LOG
    if (Microsoft.PowerShell.Management\Test-Path -LiteralPath $log) {
        $result.trace = @(Microsoft.PowerShell.Management\Get-Content -LiteralPath $log)
        $result.cli = @($result.trace | Where-Object { $_.StartsWith('start|') }).Count
    }
    $result.envRestored = $env:NIKON_D810_SDK_ROOT -eq $expectedSdkRoot
    $result.nativePreferenceRestored = $PSNativeCommandUseErrorActionPreference -eq $expectedNativePreference
    $result.afterMarker = $true
    $result | ConvertTo-Json -Depth 5 | Microsoft.PowerShell.Management\Set-Content -LiteralPath $ResultPath
}
exit $result.exitCode
'@)
    Assert-Parses $path
    $path
}

function Invoke-Child([string]$Name, [hashtable]$Parameters, [hashtable]$Environment, [bool]$NativePreference = $false) {
    $caseRoot = Join-Path $script:root $Name
    $null = [IO.Directory]::CreateDirectory($caseRoot)
    $payloadPath = Join-Path $caseRoot 'payload.json'
    $resultPath = Join-Path $caseRoot 'result.json'
    $fakeEnvironment = @{} + $script:defaultEnvironment
    foreach ($key in $Environment.Keys) { $fakeEnvironment[$key] = $Environment[$key] }
    $fakeEnvironment.A0_READINESS_FAKE_LOG = Join-Path $caseRoot 'fake-cli.log'
    $payload = [ordered]@{
        Entry = [IO.Path]::GetFullPath($ReadinessScriptPath)
        Parameters = $Parameters; FakeEnvironment = $fakeEnvironment
        WorkRoot = $script:root; NativeErrorPreference = $NativePreference
    }
    [IO.File]::WriteAllText($payloadPath, ($payload | ConvertTo-Json -Depth 6))
    $info = [Diagnostics.ProcessStartInfo]::new()
    # Use this PowerShell installation; PATH can contain multiple pwsh binaries.
    $info.FileName = Join-Path $PSHOME 'pwsh.exe'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in @('-NoProfile', '-NonInteractive', '-File', $script:wrapper, '-PayloadPath', $payloadPath, '-ResultPath', $resultPath)) {
        $info.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(20000)) {
        # No deletion, retry or kill of a potentially unfinished child.
        throw "$Name child timeout; PID $($process.Id), artifacts retained at $caseRoot"
    }
    $output = $stdout.GetAwaiter().GetResult()
    $errorOutput = $stderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $caseRoot 'stdout.txt'), $output)
    [IO.File]::WriteAllText((Join-Path $caseRoot 'stderr.txt'), $errorOutput)
    Assert-That (Test-Path -LiteralPath $resultPath -PathType Leaf) "$Name child marker missing: $errorOutput"
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json -AsHashtable
    Assert-That ($result.afterMarker -and $result.pnpIsFunction) "$Name fake PnP/finally marker missing"
    Assert-That ($process.ExitCode -eq $result.exitCode) "$Name child native exit differs from recorded exit"
    $process.Dispose()
    $result.stdout = $output.Replace([string][char]13, '')
    $result.stderr = $errorOutput + $result.error
    $result.name = $Name
    $script:results.Add($result)
    $result
}

function Assert-Outcome($Result, [int]$ExitCode, [string]$State, [int]$Pnp, [int]$Cli) {
    $name = $Result.name
    # First assertion gives baseline RED its intended safety reason.
    Assert-That ($Result.pnp -eq $Pnp -and $Result.cli -eq $Cli) "$name unexpected PnP/CLI calls: $($Result.pnp)/$($Result.cli); expected $Pnp/$Cli"
    Assert-That ($Result.exitCode -eq $ExitCode) "$name expected exit $ExitCode, actual $($Result.exitCode)"
    Assert-That ($Result.stdout -match ('(?m)^Phase0Preflight: ' + [regex]::Escape($State) + '$')) "$name wrong status"
    Assert-That $Result.envRestored "$name SDK environment not restored"
    Assert-That $Result.nativePreferenceRestored "$name native error preference not restored"
    Assert-That ($Result.stdout -notmatch 'SYNTHETIC-RAW-DEVICE-ID-DO-NOT-PRINT' -and $Result.stderr -notmatch 'SYNTHETIC-RAW-DEVICE-ID-DO-NOT-PRINT') "$name raw identifier leaked"
    Assert-That ($Result.stdout -notmatch [regex]::Escape($script:root) -and $Result.stdout -notmatch 'C:\\fake-vs') "$name local path leaked"
    Assert-That ([string]::IsNullOrWhiteSpace($Result.stderr)) "$name unexpected stderr"
    Assert-That ($Result.trace.Count -eq $Cli * 2) "$name unfinished or duplicate CLI calls"
    if ($Pnp -gt 0) { Assert-That ($Result.leaseHeld -eq $true) "$name verified executable write lease was not held" }
    $commands = @(('preflight --stage ' + $script:currentStage), 'inventory --transport sdk', 'inventory --transport wpd')
    for ($index = 0; $index -lt $Cli; $index++) {
        $command = $commands[$index]
        Assert-That ($Result.trace[2 * $index] -ceq "start|$($script:fake.Release)|$command") "$name wrong executable/command start order"
        Assert-That ($Result.trace[2 * $index + 1] -ceq "end|$($script:fake.Release)|$command") "$name missing completion before next transport"
    }
    Write-Output "PASS: $name (exit $ExitCode; PnP $Pnp; CLI $Cli)"
}

function Assert-Field($Result, [string]$Name, [string]$Value) {
    Assert-That ($Result.stdout -match ('(?m)^' + [regex]::Escape($Name) + '\s*:\s*' + [regex]::Escape($Value) + '$')) "$($Result.name) missing field $Name=$Value"
}

Assert-That $IsWindows 'This regression requires Windows and PowerShell 7.4+'
Assert-That (Test-Path -LiteralPath $ReadinessScriptPath -PathType Leaf) 'Readiness entry missing'
Assert-Parses $ReadinessScriptPath
$entryText = Get-Content -LiteralPath $ReadinessScriptPath -Raw
Assert-That ($entryText -notmatch '(?i)([\w.]+\\Get-PnpDevice|\[DllImport|Add-Type.*PInvoke)') 'Hardware bypass outside fake boundary found'
$script:root = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('a0-readiness-script-' + [guid]::NewGuid().ToString('N'))))
$null = [IO.Directory]::CreateDirectory($script:root)
$script:results = [Collections.Generic.List[object]]::new()
$script:defaultEnvironment = @{
    A0_FAKE_PNP_COUNT = '1'; A0_FAKE_PREFLIGHT_FAIL = '0'
    A0_FAKE_SDK_COUNT = '1'; A0_FAKE_SDK_BOUND = '1'; A0_FAKE_SDK_UNBOUND = '0'; A0_FAKE_SDK_FAIL = '0'
    A0_FAKE_WPD_COUNT = '1'; A0_FAKE_WPD_BOUND = '1'; A0_FAKE_WPD_UNBOUND = '0'; A0_FAKE_WPD_FAIL = '0'
}
Write-Output "Fake-only artifacts: $script:root"
try {
    $script:fake = New-FakeCli $script:root
    $script:wrapper = New-ChildWrapper $script:root
    $sdk = Join-Path $script:root 'sdk'
    $null = [IO.Directory]::CreateDirectory($sdk)
    $script:currentStage = 'single'
    $legacy = @{ Stage = 'Single'; SdkRoot = $sdk; BuildRoot = $script:root }
    Assert-Outcome (Invoke-Child 'missing-identity-legacy' $legacy @{}) 1 'BLOCKED' 0 0
    if ($BaselineGuardOnly) { return }

    $common = @{} + $legacy
    $common.Phase0ExecutablePath = $fake.Release
    $common.ExpectedPhase0Sha256 = Get-Sha256 $fake.Release
    Assert-Outcome (Invoke-Child 'no-arguments' @{} @{}) 1 'BLOCKED' 0 0
    $single = Invoke-Child 'single-explicit-release' $common @{}
    Assert-Outcome $single 0 'READY' 1 3
    Assert-Field $single 'Phase0ExecutableSelection' 'VerifiedExplicit'
    Assert-Field $single 'Phase0ExecutableAlias' 'PHASE0-CLI'
    Assert-Field $single 'Phase0ExecutableSHA256' $common.ExpectedPhase0Sha256
    Assert-Field $single 'Phase0SourceCommitVerified' 'False'
    Assert-Field $single 'ExecutablePathPrinted' 'False'
    $upper = @{} + $common
    $upper.ExpectedPhase0Sha256 = $upper.ExpectedPhase0Sha256.ToUpperInvariant()
    Assert-Outcome (Invoke-Child 'uppercase-digest' $upper @{}) 0 'READY' 1 3

    $dual = @{} + $common
    $dual.Stage = 'Dual'
    $two = @{ A0_FAKE_PNP_COUNT = '2'; A0_FAKE_SDK_COUNT = '2'; A0_FAKE_SDK_BOUND = '2'; A0_FAKE_WPD_COUNT = '2'; A0_FAKE_WPD_BOUND = '2' }
    $script:currentStage = 'dual'
    Assert-Outcome (Invoke-Child 'dual-ready' $dual $two) 0 'READY' 1 3
    $unboundDual = @{} + $two
    $unboundDual.A0_FAKE_SDK_BOUND = '1'
    $unboundDual.A0_FAKE_SDK_UNBOUND = '1'
    Assert-Outcome (Invoke-Child 'dual-unbound' $dual $unboundDual) 2 'READY_FOR_IDENTITY_BINDING' 1 3
    $mismatch = @{} + $two
    $mismatch.A0_FAKE_WPD_COUNT = '1'
    Assert-Outcome (Invoke-Child 'dual-count-mismatch' $dual $mismatch) 1 'BLOCKED' 1 3
    $script:currentStage = 'single'
    Assert-Outcome (Invoke-Child 'single-unbound' $common @{ A0_FAKE_SDK_BOUND = '0'; A0_FAKE_SDK_UNBOUND = '1' }) 2 'READY_FOR_IDENTITY_BINDING' 1 3
    Assert-Outcome (Invoke-Child 'pnp-count-mismatch' $common @{ A0_FAKE_PNP_COUNT = '0' }) 1 'BLOCKED' 1 3
    Assert-Outcome (Invoke-Child 'preflight-nonzero' $common @{ A0_FAKE_PREFLIGHT_FAIL = '1'; NIKON_D810_SDK_ROOT = 'pre-existing-synthetic' }) 1 'BLOCKED' 1 1
    foreach ($preference in @($false, $true)) {
        Assert-Outcome (Invoke-Child "sdk-nonzero-$preference" $common @{ A0_FAKE_SDK_FAIL = '1'; NIKON_D810_SDK_ROOT = 'pre-existing-synthetic' } $preference) 1 'BLOCKED' 1 3
    }
    Assert-Outcome (Invoke-Child 'wpd-nonzero' $common @{ A0_FAKE_WPD_FAIL = '1' }) 1 'BLOCKED' 1 3

    $directoryExe = Join-Path $script:root 'directory.exe'
    $null = [IO.Directory]::CreateDirectory($directoryExe)
    $badCases = @(
        @{ Name = 'missing-path'; Key = 'Phase0ExecutablePath'; Value = ''; Selection = 'MissingExecutablePath' }
        @{ Name = 'missing-hash'; Key = 'ExpectedPhase0Sha256'; Value = ''; Selection = 'MissingExpectedSha256' }
        @{ Name = 'bad-hex'; Key = 'ExpectedPhase0Sha256'; Value = ('g' * 64); Selection = 'InvalidExpectedSha256' }
        @{ Name = 'short-hash'; Key = 'ExpectedPhase0Sha256'; Value = ('0' * 63); Selection = 'InvalidExpectedSha256' }
        @{ Name = 'long-hash'; Key = 'ExpectedPhase0Sha256'; Value = ('0' * 65); Selection = 'InvalidExpectedSha256' }
        @{ Name = 'newline-hash'; Key = 'ExpectedPhase0Sha256'; Value = ($common.ExpectedPhase0Sha256 + [char]10); Selection = 'InvalidExpectedSha256' }
        @{ Name = 'mismatched-hash'; Key = 'ExpectedPhase0Sha256'; Value = ('0' * 64); Selection = 'Sha256Mismatch' }
        @{ Name = 'missing-file'; Key = 'Phase0ExecutablePath'; Value = (Join-Path $script:root 'missing.exe'); Selection = 'ExecutableUnavailable' }
        @{ Name = 'directory-not-exe'; Key = 'Phase0ExecutablePath'; Value = $directoryExe; Selection = 'ExecutableUnavailable' }
        @{ Name = 'relative-path'; Key = 'Phase0ExecutablePath'; Value = '.\Release\A0CameraStitcher.Phase0.exe'; Selection = 'InvalidExecutablePath' }
        @{ Name = 'drive-relative-path'; Key = 'Phase0ExecutablePath'; Value = 'C:fake.exe'; Selection = 'InvalidExecutablePath' }
        @{ Name = 'unc-path'; Key = 'Phase0ExecutablePath'; Value = '\\not-contacted\share\fake.exe'; Selection = 'InvalidExecutablePath' }
        @{ Name = 'not-executable-extension'; Key = 'Phase0ExecutablePath'; Value = (Join-Path $script:root 'Program.cs'); Selection = 'InvalidExecutablePath' }
    )
    foreach ($case in $badCases) {
        $parameters = @{} + $common
        $parameters[$case.Key] = $case.Value
        $result = Invoke-Child $case.Name $parameters @{ NIKON_D810_SDK_ROOT = 'pre-existing-synthetic' }
        Assert-Outcome $result 1 'BLOCKED' 0 0
        Assert-Field $result 'Phase0ExecutableSelection' $case.Selection
    }
    $lock = [IO.File]::Open($fake.Release, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try {
        $locked = Invoke-Child 'unreadable-executable' $common @{}
        Assert-Outcome $locked 1 'BLOCKED' 0 0
        Assert-Field $locked 'Phase0ExecutableSelection' 'ExecutableUnreadable'
    } finally { $lock.Dispose() }
    Write-Output "PASS: $($script:results.Count) fake-only public-entry cases; no physical-device verification"
} finally {
    # Keep logs on success and failure. No recursive deletion or cleanup race with
    # an unfinished child. These test-only files are not product/SDK evidence.
    $script:results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $script:root 'results.json')
    Write-Output "Artifacts retained: $script:root"
}
