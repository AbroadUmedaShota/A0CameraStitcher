# Shared by the Windows workflow and the local classifier regression test.
# Keep this allowlist deliberately narrow: only explicitly named Markdown
# documents can use the lightweight lane. Machine-consumed or unclassified
# paths must remain on full validation.

function Get-CiValidationScope {
    [CmdletBinding()]
    param(
        [AllowNull()]
        [string[]]$ChangedPaths
    )

    $paths = @($ChangedPaths)
    if ($paths.Count -eq 0) {
        return [pscustomobject]@{
            RunFull = $true
            Reason = 'unknown or empty changed path set'
        }
    }

    foreach ($path in $paths) {
        if ([string]::IsNullOrWhiteSpace($path)) {
            return [pscustomobject]@{
                RunFull = $true
                Reason = 'unknown or empty changed path'
            }
        }

        $normalized = $path.Trim().Replace('\', '/')

        # A relative Git path is expected. Reject path-like input that cannot
        # be classified safely instead of allowing it into the doc lane.
        if ($normalized.StartsWith('/') -or
            $normalized -match '^[A-Za-z]:/' -or
            $normalized -match '(^|/)\.\.?(?:/|$)') {
            return [pscustomobject]@{
                RunFull = $true
                Reason = "unknown path: $path"
            }
        }

        # These JSON Schemas are consumed by scripts/Test-M2PreGateAssets.ps1
        # and runtime contracts. Keep the whole machine-consumed subtree full,
        # including a Markdown file added there as a future schema asset.
        if ($normalized -match '^docs/schemas(?:/|$)') {
            return [pscustomobject]@{
                RunFull = $true
                Reason = 'machine-consumed docs/schemas artifact changed'
            }
        }

        # Markdown is the only explicit non-executable document allowlist.
        # JSON, YAML, SVG, scripts, binaries, and other extensions stay full.
        if ($normalized -notmatch '^(?:[^/]+/)*[^/]+\.(?:md|markdown)$') {
            return [pscustomobject]@{
                RunFull = $true
                Reason = "code, build, test, workflow, machine artifact, or unknown path changed: $path"
            }
        }
    }

    return [pscustomobject]@{
        RunFull = $false
        Reason = 'pure Markdown documentation-only PR'
    }
}
