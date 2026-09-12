[CmdletBinding()]
param(
    [ValidateSet('Single', 'Dual')]
    [string]$Stage = 'Single',

    [string]$SdkRoot = (Join-Path $PSScriptRoot '..\.tools\nikon\d810-remote-sdk'),

    # Retained for old callers only; never used to discover/select an executable.
    [string]$BuildRoot = (Join-Path $PSScriptRoot '..\build'),

    [string]$Phase0ExecutablePath = '',

    [string]$ExpectedPhase0Sha256 = ''
)

$ErrorActionPreference = 'Stop'
# Native exit codes are classified below, independent of the caller's preference.
# Invocation with & gives this assignment script scope and preserves the caller.
$PSNativeCommandUseErrorActionPreference = $false

# No Mandatory parameter prompts: even unattended callers with missing inputs
# must stop before PnP, preflight or either inventory transport is accessed.
$phase0Lease = $null
$phase0Sha256 = $null
$selection = 'MissingExecutablePath'
try {
    if ([string]::IsNullOrWhiteSpace($Phase0ExecutablePath)) { throw $selection }
    $selection = 'MissingExpectedSha256'
    if ([string]::IsNullOrWhiteSpace($ExpectedPhase0Sha256)) { throw $selection }
    $selection = 'InvalidExpectedSha256'
    if ($ExpectedPhase0Sha256 -cnotmatch '\A[0-9a-fA-F]{64}\z') { throw $selection }
    $selection = 'InvalidExecutablePath'
    # Drive-absolute local paths only; no PATH lookup, drive-relative path or UNC.
    if ($Phase0ExecutablePath -notmatch '\A[A-Za-z]:[\\/]' -or
        [System.IO.Path]::GetExtension($Phase0ExecutablePath) -ine '.exe') { throw $selection }
    $phase0Exe = [System.IO.Path]::GetFullPath($Phase0ExecutablePath)
    $selection = 'ExecutableUnavailable'
    $file = [System.IO.FileInfo]::new($phase0Exe)
    if (-not $file.Exists) { throw $selection }
    # A mutable junction/symlink must not redirect the path after it was hashed.
    $selection = 'ReparsePathRejected'
    $component = $file
    while ($null -ne $component) {
        if (($component.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) { throw $selection }
        $component = if ($component -is [System.IO.FileInfo]) { $component.Directory } else { $component.Parent }
    }
    $selection = 'ExecutableUnreadable'
    # Retain the verified file handle until all commands finish. This also
    # denies writes/replacement while Windows launches the selected executable.
    $phase0Lease = [System.IO.File]::Open($phase0Exe, 'Open', 'Read', 'Read')
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $phase0Sha256 = [BitConverter]::ToString($sha256.ComputeHash($phase0Lease)).Replace('-', '').ToLowerInvariant()
    } finally { $sha256.Dispose() }
    $selection = 'Sha256Mismatch'
    if (-not [string]::Equals($phase0Sha256, $ExpectedPhase0Sha256, [StringComparison]::OrdinalIgnoreCase)) { throw $selection }
    $selection = 'VerifiedExplicit'
} catch {
    if ($null -ne $phase0Lease) { $phase0Lease.Dispose() }
    [pscustomobject][ordered]@{
        Stage = $Stage
        Phase0ExecutableSelection = $selection
        Phase0ExecutableAlias = 'PHASE0-CLI'
        Phase0ExecutableSHA256 = $phase0Sha256
        Phase0SourceCommitVerified = $false
        ExecutablePathPrinted = $false
    } | Format-List
    Write-Output 'Phase0Preflight: BLOCKED'
    Write-Output 'Executable identity was not verified. No PnP, SDK or WPD operation was performed.'
    exit 1
}

try {
$vsRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'
$vsDevCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$msvcRoot = Join-Path $vsRoot 'VC\Tools\MSVC'
$vsCmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'

$msvcToolset = Get-ChildItem -LiteralPath $msvcRoot -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    Select-Object -First 1
$clPath = if ($msvcToolset) {
    Join-Path $msvcToolset.FullName 'bin\Hostx64\x64\cl.exe'
} else {
    $null
}
$msvcReady = $null -ne $clPath -and (Test-Path -LiteralPath $clPath) -and (Test-Path -LiteralPath $vsDevCmd)

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmakePath = if ($cmakeCommand) {
    $cmakeCommand.Source
} elseif (Test-Path -LiteralPath $vsCmake) {
    $vsCmake
} else {
    $null
}
$cmakeReady = $null -ne $cmakePath

$resolvedSdkRoot = [System.IO.Path]::GetFullPath($SdkRoot)
$sdkRootReady = Test-Path -LiteralPath $resolvedSdkRoot -PathType Container

$cliPresent = $true # Established by the explicit identity gate above.

$d810Nodes = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
    $_.FriendlyName -match '(^|\s)D810($|\s)|Nikon.*D810|D810.*Nikon'
})

$requiredBodies = if ($Stage -eq 'Dual') { 2 } else { 1 }
$cliReady = $false
$cliExit = $null
$sdkInventoryReady = $false
$sdkInventoryExit = $null
$sdkCameraCount = 0
$sdkBoundCameraCount = 0
$sdkUnboundCameraCount = 0
$wpdInventoryReady = $false
$wpdInventoryExit = $null
$wpdCameraCount = 0
$wpdBoundCameraCount = 0
$wpdUnboundCameraCount = 0
$identityBindingsReady = $false
if ($cliPresent -and $sdkRootReady) {
    $previousSdkRoot = $env:NIKON_D810_SDK_ROOT
    try {
        $env:NIKON_D810_SDK_ROOT = $resolvedSdkRoot
        # Never forward raw native output (which can contain paths/identifiers).
        $preflightOutput = @(& $phase0Exe preflight --stage $Stage.ToLowerInvariant() 2>&1)
        $cliExit = $LASTEXITCODE
        $cliReady = $cliExit -eq 0
        if ($cliReady) {
            $inventoryOutput = @(& $phase0Exe inventory --transport sdk 2>&1)
            $sdkInventoryExit = $LASTEXITCODE
            foreach ($line in $inventoryOutput) {
                if ($line -match '^CameraCount:\s*(\d+)\s*$') {
                    $sdkCameraCount = [int]$Matches[1]
                } elseif ($line -match '^BoundCameraCount:\s*(\d+)\s*$') {
                    $sdkBoundCameraCount = [int]$Matches[1]
                } elseif ($line -match '^UnboundCameraCount:\s*(\d+)\s*$') {
                    $sdkUnboundCameraCount = [int]$Matches[1]
                }
            }
            $sdkInventoryReady = $sdkInventoryExit -eq 0 -and $sdkCameraCount -eq $requiredBodies

            $inventoryOutput = @(& $phase0Exe inventory --transport wpd 2>&1)
            $wpdInventoryExit = $LASTEXITCODE
            foreach ($line in $inventoryOutput) {
                if ($line -match '^CameraCount:\s*(\d+)\s*$') {
                    $wpdCameraCount = [int]$Matches[1]
                } elseif ($line -match '^BoundCameraCount:\s*(\d+)\s*$') {
                    $wpdBoundCameraCount = [int]$Matches[1]
                } elseif ($line -match '^UnboundCameraCount:\s*(\d+)\s*$') {
                    $wpdUnboundCameraCount = [int]$Matches[1]
                }
            }
            $wpdInventoryReady = $wpdInventoryExit -eq 0 -and $wpdCameraCount -eq $requiredBodies
            $identityBindingsReady = $sdkInventoryReady -and $wpdInventoryReady -and
                $sdkBoundCameraCount -eq $requiredBodies -and $sdkUnboundCameraCount -eq 0 -and
                $wpdBoundCameraCount -eq $requiredBodies -and $wpdUnboundCameraCount -eq 0
        }
    } finally {
        $env:NIKON_D810_SDK_ROOT = $previousSdkRoot
    }
}

$checks = [ordered]@{
    Stage = $Stage
    WindowsX64 = [Environment]::Is64BitOperatingSystem
    MsvcX64 = $msvcReady
    MsvcToolset = if ($msvcToolset) { $msvcToolset.Name } else { $null }
    CMake = $cmakeReady
    CMakePathPrinted = $false
    SdkRootPresent = $sdkRootReady
    Phase0CliPresent = $cliPresent
    Phase0ExecutableSelection = $selection
    Phase0ExecutableAlias = 'PHASE0-CLI'
    Phase0ExecutableSHA256 = $phase0Sha256
    Phase0SourceCommitVerified = $false
    ExecutablePathPrinted = $false
    LicensedAdapterPreflight = $cliReady
    LicensedSdkInventory = $sdkInventoryReady
    LicensedSdkCameraCount = $sdkCameraCount
    LicensedSdkBoundCameraCount = $sdkBoundCameraCount
    LicensedSdkUnboundCameraCount = $sdkUnboundCameraCount
    WpdInventory = $wpdInventoryReady
    WpdCameraCount = $wpdCameraCount
    WpdBoundCameraCount = $wpdBoundCameraCount
    WpdUnboundCameraCount = $wpdUnboundCameraCount
    CrossTransportIdentityBindings = $identityBindingsReady
    MatchingD810PnpNodes = $d810Nodes.Count
    RequiredCameraBodies = $requiredBodies
}

[pscustomobject]$checks | Format-List

$inventoryReady = $checks.WindowsX64 -and $checks.MsvcX64 -and $checks.CMake -and
    $checks.SdkRootPresent -and $checks.Phase0CliPresent -and $checks.LicensedAdapterPreflight -and
    $checks.LicensedSdkInventory -and $checks.WpdInventory -and
    $checks.MatchingD810PnpNodes -eq $checks.RequiredCameraBodies
$ready = $inventoryReady -and $checks.CrossTransportIdentityBindings

if ($ready) {
    Write-Output 'Phase0Preflight: READY'
    Write-Output 'Required physical body count and explicit SDK/WPD identity bindings were confirmed.'
    exit 0
}

if ($inventoryReady) {
    Write-Output 'Phase0Preflight: READY_FOR_IDENTITY_BINDING'
    Write-Output 'Required bodies were enumerated, but explicit one-body-at-a-time SDK/WPD bindings are incomplete.'
    Write-Output 'Inventory is read-only and does not assign CAM-A/B from enumeration order.'
    exit 2
}

Write-Output 'Phase0Preflight: BLOCKED'
Write-Output 'No camera serial number, instance ID, or SDK content was printed.'
exit 1
} catch {
    # Report a stable category, not exception messages containing local paths or
    # native diagnostics. The nested finally restores the SDK environment first.
    Write-Output 'Phase0ExecutableSelection: VerifiedExplicit'
    Write-Output "Phase0ExecutableSHA256: $phase0Sha256"
    Write-Output 'Phase0SourceCommitVerified: False'
    Write-Output 'Phase0Preflight: BLOCKED'
    Write-Output 'ReadinessCheckFailed. No raw exception or device identifiers were printed.'
    exit 1
} finally {
    $phase0Lease.Dispose()
}
