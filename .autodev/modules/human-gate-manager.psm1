Set-StrictMode -Version Latest

function Get-AutoDevHumanGates {
    [CmdletBinding()]
    param(
        [Parameter()]
        [string] $RepositoryRoot = (Get-Location).Path
    )

    $gatePath = Join-Path $RepositoryRoot '.autodev\human-gates\open.json'
    if (-not (Test-Path -LiteralPath $gatePath -PathType Leaf)) {
        throw "Human gate file was not found: $gatePath"
    }

    $document = Get-Content -LiteralPath $gatePath -Raw | ConvertFrom-Json
    return @($document.gates)
}

function Test-AutoDevHumanGateOpen {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string] $Id,

        [Parameter()]
        [string] $RepositoryRoot = (Get-Location).Path
    )

    return [bool](Get-AutoDevHumanGates -RepositoryRoot $RepositoryRoot | Where-Object { $_.id -eq $Id -and $_.status -eq 'open' })
}

Export-ModuleMember -Function Get-AutoDevHumanGates, Test-AutoDevHumanGateOpen
