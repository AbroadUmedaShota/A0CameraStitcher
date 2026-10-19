[CmdletBinding()]
param([ValidateRange(0,3600)][int]$Seconds=0)
$ErrorActionPreference='Stop'
$previewExe=Join-Path $PSScriptRoot '../build/reference/Release/A0CameraStitcher.WpdDualPreview.exe'
if(-not (Test-Path -LiteralPath $previewExe -PathType Leaf)) {
    throw 'Build A0CameraStitcher.WpdDualPreview in the SDK-free build/reference configuration first. See docs/WPD_DUAL_PREVIEW.md.'
}
# This interactive development preview holds the hardware-control lease itself.
# Esc/window close stops both cameras; 0 means display until window close.
& $previewExe --seconds $Seconds
exit $LASTEXITCODE
