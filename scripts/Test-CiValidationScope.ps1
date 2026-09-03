[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..'))
)

$ErrorActionPreference = 'Stop'

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

try {
    . (Join-Path $PSScriptRoot 'Resolve-CiValidationScope.ps1')

    function Assert-Scope {
        param(
            [Parameter(Mandatory)][string]$CaseName,
            [AllowNull()][string[]]$ChangedPaths,
            [Parameter(Mandatory)][bool]$ExpectedRunFull
        )

        $scope = Get-CiValidationScope -ChangedPaths $ChangedPaths
        Assert-Condition ($scope.RunFull -eq $ExpectedRunFull) "$CaseName classified as run_full=$($scope.RunFull); expected run_full=$ExpectedRunFull. Reason: $($scope.Reason)"
    }

    Assert-Scope -CaseName 'pure Markdown' -ChangedPaths @('README.md', 'docs/CI_COST_PROFILE.md', 'docs/notes.markdown') -ExpectedRunFull $false
    Assert-Scope -CaseName 'docs/schemas JSON artifact' -ChangedPaths @('docs/schemas/rig-profile.schema.json') -ExpectedRunFull $true
    Assert-Scope -CaseName 'docs/schemas subtree regardless of extension' -ChangedPaths @('docs/schemas/README.md') -ExpectedRunFull $true
    Assert-Scope -CaseName 'docs plus code' -ChangedPaths @('docs/notes.md', 'src/phase0/phase0.cpp') -ExpectedRunFull $true
    Assert-Scope -CaseName 'unknown path' -ChangedPaths @('docs/config.yaml') -ExpectedRunFull $true
    Assert-Scope -CaseName 'empty changed path set' -ChangedPaths $null -ExpectedRunFull $true
    Assert-Scope -CaseName 'empty changed path entry' -ChangedPaths @('') -ExpectedRunFull $true
    Assert-Scope -CaseName 'source to docs rename with no-renames paths' -ChangedPaths @('src/phase0/phase0.cpp', 'docs/phase0.md') -ExpectedRunFull $true

    $workflowPath = Join-Path $RepositoryRoot '.github/workflows/software-ci.yml'
    $workflowText = Get-Content -Raw -LiteralPath $workflowPath
    Assert-Condition ($workflowText.Contains('git diff --name-only --no-renames')) 'The workflow must classify rename changes with git diff --no-renames.'
    Assert-Condition ($workflowText.Contains('Resolve-CiValidationScope.ps1')) 'The workflow must use the shared classifier.'

    Write-Host 'CI validation scope classifier regression tests passed.'
    exit 0
}
catch {
    Write-Error "CI validation scope classifier regression failed: $($_.Exception.Message)"
    exit 1
}
