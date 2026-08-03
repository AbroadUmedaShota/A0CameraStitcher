[CmdletBinding()]
param(
    [switch] $CheckOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$requiredFiles = @(
    '.autodev\config.yaml',
    '.autodev\state.json',
    '.autodev\requirements\normalized.json',
    '.autodev\requirements\unresolved_questions.json',
    '.autodev\plan.json'
)

$missing = @($requiredFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $repositoryRoot $_) -PathType Leaf) })
if ($missing.Count -gt 0) {
    throw "AutoDev bootstrap is incomplete: $($missing -join ', ')"
}

$state = Get-Content -LiteralPath (Join-Path $repositoryRoot '.autodev\state.json') -Raw | ConvertFrom-Json
$openGates = @(Get-Content -LiteralPath (Join-Path $repositoryRoot '.autodev\human-gates\open.json') -Raw | ConvertFrom-Json | Select-Object -ExpandProperty gates | Where-Object status -eq 'open')

[pscustomobject]@{
    Repository = $state.repo.id
    Lifecycle = $state.lifecycle
    Governance = $state.governance.mode
    OpenHumanGates = $openGates.Count
    CheckOnly = [bool]$CheckOnly
} | Format-List

if (-not $CheckOnly) {
    Write-Warning 'Automation is intentionally paused. Resolve the open Phase 0 gates before activating a heartbeat.'
}
