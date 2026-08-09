[CmdletBinding()]
param(
    [ValidateSet('Single', 'Dual')]
    [string]$Stage = 'Single',

    [string]$SdkRoot = (Join-Path $PSScriptRoot '..\.tools\nikon\d810-remote-sdk'),

    [string]$BuildRoot = (Join-Path $PSScriptRoot '..\build')
)

$ErrorActionPreference = 'Stop'

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

$phase0Candidates = @(
    (Join-Path $BuildRoot 'Debug\A0CameraStitcher.Phase0.exe'),
    (Join-Path $BuildRoot 'Release\A0CameraStitcher.Phase0.exe'),
    (Join-Path $BuildRoot 'A0CameraStitcher.Phase0.exe')
)
$phase0Exe = $phase0Candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
$cliPresent = $null -ne $phase0Exe

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
        & $phase0Exe preflight --stage $Stage.ToLowerInvariant()
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
    CMakePath = $cmakePath
    SdkRootPresent = $sdkRootReady
    Phase0CliPresent = $cliPresent
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
