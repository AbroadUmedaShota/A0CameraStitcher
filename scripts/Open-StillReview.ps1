[CmdletBinding(DefaultParameterSetName='Report')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Report')][string]$Report,
    [Parameter(Mandatory=$true,ParameterSetName='Session')][string]$Session
)
$ErrorActionPreference='Stop'
$inspectorExe=Join-Path $PSScriptRoot '../build/reference/Release/A0CameraStitcher.StillReview.exe'
if(-not (Test-Path -LiteralPath $inspectorExe -PathType Leaf)) {
    throw 'Build A0CameraStitcher.StillReview in the SDK-free build/reference configuration first.'
}
# Explicit selection only. No latest-file lookup and no camera operations.
if($PSCmdlet.ParameterSetName -eq 'Session') { & $inspectorExe --session $Session }
else { & $inspectorExe --report $Report }
exit $LASTEXITCODE
