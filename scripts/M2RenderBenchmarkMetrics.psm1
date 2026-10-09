Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-A0MetricNumber {
    param([AllowNull()]$Value, [string]$Name = 'metric', [switch]$Integer)
    $numericTypes = @([byte],[sbyte],[int16],[uint16],[int32],[uint32],[int64],[uint64],[single],[double],[decimal])
    if ($null -eq $Value -or $Value.GetType() -notin $numericTypes) { throw "$Name must be a number." }
    $number = [double]$Value
    if (-not [double]::IsFinite($number) -or $number -lt 0 -or
        ($Integer -and [Math]::Truncate($number) -ne $number)) { throw "$Name is invalid." }
    return $number
}

function Get-A0R7Percentile {
    param([Parameter(Mandatory)][object[]]$Samples, [Parameter(Mandatory)][double]$Percentile)
    if ($Samples.Count -eq 0 -or -not [double]::IsFinite($Percentile) -or
        $Percentile -lt 0 -or $Percentile -gt 100) { throw 'Percentile requires samples and a value in [0,100].' }
    [double[]]$sorted = @($Samples | ForEach-Object { Assert-A0MetricNumber $_ })
    [Array]::Sort($sorted)
    $position = ($sorted.Length - 1) * ($Percentile / 100.0)
    $lower = [int][Math]::Floor($position)
    $upper = [int][Math]::Ceiling($position)
    return $sorted[$lower] + ($position - $lower) * ($sorted[$upper] - $sorted[$lower])
}

function Get-A0MetricMaximum {
    param([Parameter(Mandatory)][object[]]$Samples)
    if ($Samples.Count -eq 0) { throw 'Maximum requires samples.' }
    $maximum = 0.0
    foreach ($sample in $Samples) { $maximum = [Math]::Max($maximum, (Assert-A0MetricNumber $sample)) }
    return $maximum
}

function ConvertTo-A0RoundedMetric {
    param([Parameter(Mandatory)][double]$RawValue, [ValidateRange(0,9)][int]$DecimalPlaces)
    $null = Assert-A0MetricNumber $RawValue
    # Native validation rounds the exact decimal spelling emitted by JSON, not
    # a binary multiplication that can turn a below-half value into a tie.
    $text = ConvertTo-Json -InputObject $RawValue -Compress
    $decimal = [decimal]::Parse($text, [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture)
    $factor = [decimal][Math]::Pow(10, $DecimalPlaces)
    if ($decimal * $factor -gt [decimal]549755813888) { throw 'Metric exceeds the exact-safe rounding range.' }
    $rounded = [decimal]::Round($decimal, $DecimalPlaces, [MidpointRounding]::AwayFromZero)
    if ($rounded * $factor -gt [decimal]549755813888) { throw 'Rounded metric exceeds the exact-safe range.' }
    return [double]$rounded
}

function New-A0ResourceMetricPair {
    param(
        [Parameter(Mandatory)][string]$MetricId,
        [Parameter(Mandatory)][ValidateSet('milliseconds','bytes')][string]$Unit,
        [Parameter(Mandatory)][ValidateSet('p50','p95','max')][string]$Statistic,
        [Parameter(Mandatory)][object[]]$Samples,
        [string]$Population = 'all10',
        [string]$MeasurementScope = 'per-run'
    )
    if ($MetricId -cnotmatch '^[a-z][a-z0-9-]{0,63}$' -or $Samples.Count -eq 0) { throw 'Invalid metric identity or empty samples.' }
    if ($Unit -eq 'bytes') {
        foreach ($sample in $Samples) { $null = Assert-A0MetricNumber $sample 'bytes' -Integer }
    }
    $places = if ($Unit -eq 'bytes') { 0 } else { 2 }
    $aggregation = [ordered]@{
        method = 'Maximum'; percentile = $null; algorithm = 'SortedLast'
        percentileInterpolation = $null; medianEvenRule = $null
    }
    if ($Statistic -eq 'max') { $raw = Get-A0MetricMaximum -Samples $Samples }
    else {
        $percentile = if ($Statistic -eq 'p50') { 50 } else { 95 }
        $raw = Get-A0R7Percentile -Samples $Samples -Percentile $percentile
        $aggregation.method = 'Percentile'; $aggregation.percentile = $percentile
        $aggregation.algorithm = 'SortedOrderStatistic'; $aggregation.percentileInterpolation = 'LinearR7'
    }
    $rounding = [ordered]@{ mode = 'HalfAwayFromZero'; decimalPlaces = $places; order = 'AggregateThenRound' }
    $definition = [ordered]@{
        schemaVersion = 'a0.stitch-metric-definition.v1'; definitionVersion = 1; metricId = $MetricId
        domain = 'Resource'; unit = $Unit; coordinateSystem = $null; mask = $null; sampling = $null
        aggregation = $aggregation; rounding = $rounding; boundary = $null
        confidence = [ordered]@{ method = 'None'; level = $null }
        uncertainty = [ordered]@{ method = 'None'; unitMode = 'SameAsMetric' }
        outcomePolicy = [ordered]@{
            allowedFailureCodes = [ordered]@{
                NoResult = @('zero-valid-results'); NotApplicable = @('metric-not-applicable')
                Invalid = @('invalid-source-contract')
            }
            noResultRule = 'ZeroValidSamples'
        }
    }
    $result = [ordered]@{
        schemaVersion = 'a0.stitch-metric-result.v1'; definitionVersion = 1; metricId = $MetricId
        metricDomain = 'Resource'; outcome = 'Success'; rawValue = [double]$raw
        value = ConvertTo-A0RoundedMetric -RawValue $raw -DecimalPlaces $places
        sampleCount = $Samples.Count; rounding = $rounding
        confidence = [ordered]@{ method = 'None'; level = $null; lower = $null; upper = $null }
        uncertainty = [ordered]@{ method = 'None'; value = $null }; failureCode = $null
    }
    return [pscustomobject]@{ population = $Population; measurementScope = $MeasurementScope; definition = $definition; result = $result }
}

function Assert-A0BenchmarkRun {
    param([Parameter(Mandatory)]$Record)
    if ($Record.schema -cne 'a0.m2.render-benchmark-run.v1') { throw 'Unsupported native benchmark schema.' }
    $expectedRecordFields = @('schema','inputSha256','outputWidth','outputHeight','wicComponentVersion','wicDllVersion','runs','allHashesEqual')
    if (@(Compare-Object @($Record.PSObject.Properties.Name) $expectedRecordFields).Count -ne 0) { throw 'Unexpected native benchmark fields.' }
    if ($Record.allHashesEqual -isnot [bool]) { throw 'Native determinism flag must be boolean.' }
    if (@($Record.inputSha256).Count -ne 2 -or @($Record.runs).Count -ne 10) { throw 'Expected two input hashes and ten runs.' }
    foreach ($hash in $Record.inputSha256) {
        if ($hash -isnot [string] -or $hash -cnotmatch '^[0-9a-fA-F]{64}$') { throw 'Invalid input SHA-256.' }
    }
    foreach ($dimension in @('outputWidth','outputHeight')) {
        $number = Assert-A0MetricNumber $Record.$dimension $dimension -Integer
        if ($number -lt 1 -or $number -gt 32768) { throw 'Invalid output dimension.' }
    }
    foreach ($version in @('wicComponentVersion','wicDllVersion')) {
        if ($Record.$version -isnot [string] -or $Record.$version -notmatch '^\d+(\.\d+){1,3}$') { throw 'Missing WIC version evidence.' }
    }
    $previousWorkingSet = 0.0; $previousCommit = 0.0
    for ($index = 0; $index -lt 10; $index++) {
        $run = $Record.runs[$index]
        $expectedRunFields = @('iteration','phase','decodeMilliseconds','renderMilliseconds','encodeMilliseconds','verifyHashMilliseconds','totalMilliseconds','outputSizeBytes','outputSha256','peakWorkingSetBytes','peakCommitBytes')
        if (@(Compare-Object @($run.PSObject.Properties.Name) $expectedRunFields).Count -ne 0) { throw 'Unexpected native run fields.' }
        if ((Assert-A0MetricNumber $run.iteration 'iteration' -Integer) -ne $index + 1) { throw 'Invalid run order.' }
        $phase = if ($index -eq 0) { 'process-first' } else { 'warm' }
        if ($run.phase -cne $phase) { throw 'Invalid cold/warm classification.' }
        foreach ($field in @('decodeMilliseconds','renderMilliseconds','encodeMilliseconds','verifyHashMilliseconds','totalMilliseconds')) {
            $null = Assert-A0MetricNumber $run.$field $field
        }
        foreach ($field in @('outputSizeBytes','peakWorkingSetBytes','peakCommitBytes')) {
            $number = Assert-A0MetricNumber $run.$field $field -Integer
            if ($number -lt 1 -or $number -gt 549755813888) { throw 'Invalid byte measurement.' }
        }
        if ($run.outputSha256 -isnot [string] -or $run.outputSha256 -cnotmatch '^[0-9a-fA-F]{64}$') { throw 'Invalid output SHA-256.' }
        if ($run.peakWorkingSetBytes -lt $previousWorkingSet -or $run.peakCommitBytes -lt $previousCommit) {
            throw 'Process-lifetime peak must not decrease.'
        }
        $previousWorkingSet = $run.peakWorkingSetBytes; $previousCommit = $run.peakCommitBytes
    }
    $hashesEqual = @($Record.runs.outputSha256 | ForEach-Object { $_.ToLowerInvariant() } | Select-Object -Unique).Count -eq 1
    if ($Record.allHashesEqual -ne $hashesEqual) { throw 'Native determinism flag contradicts run hashes.' }
}

function New-A0BenchmarkMetrics {
    param([Parameter(Mandatory)]$Record)
    Assert-A0BenchmarkRun $Record
    foreach ($population in @('all10','warm9')) {
        $runs = if ($population -eq 'all10') { @($Record.runs) } else { @($Record.runs | Select-Object -Skip 1) }
        foreach ($stage in @('decode','render','encode','verifyHash','total')) {
            $field = "${stage}Milliseconds"
            $samples = @($runs | ForEach-Object { $_.$field })
            foreach ($statistic in @('p50','p95','max')) {
                $id = "$($stage.ToLowerInvariant())-duration-$population-$statistic"
                New-A0ResourceMetricPair -MetricId $id -Unit milliseconds -Statistic $statistic -Samples $samples -Population $population
            }
        }
        foreach ($field in @('peakWorkingSetBytes','peakCommitBytes','outputSizeBytes')) {
            $name = switch ($field) {
                peakWorkingSetBytes { 'peak-working-set' }; peakCommitBytes { 'peak-commit' }; outputSizeBytes { 'output-size' }
            }
            $scope = if ($field -eq 'outputSizeBytes') { 'per-run' } else { 'process-lifetime-cumulative-observations' }
            New-A0ResourceMetricPair -MetricId "$name-$population-max" -Unit bytes -Statistic max -Samples @($runs | ForEach-Object { $_.$field }) -Population $population -MeasurementScope $scope
        }
    }
}

function Initialize-A0BenchmarkNativePaths {
    if ('A0.M2Benchmark.NativePaths' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace A0.M2Benchmark {
    public static class NativePaths {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern SafeFileHandle CreateFileW(string path, uint access, uint sharing,
            IntPtr security, uint creation, uint flags, IntPtr template);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFinalPathNameByHandleW(SafeFileHandle handle,
            StringBuilder path, uint length, uint flags);
        [StructLayout(LayoutKind.Sequential)]
        private struct FileInformation {
            public uint Attributes;
            public System.Runtime.InteropServices.ComTypes.FILETIME Created, Accessed, Written;
            public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetFileInformationByHandle(SafeFileHandle handle, out FileInformation info);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern uint GetShortPathNameW(string path, StringBuilder shortPath, uint length);

        private static string TryDirectory(string path, out int error) {
            // READ_ATTRIBUTES, share read/write/delete, OPEN_EXISTING, directory semantics.
            using (SafeFileHandle handle = CreateFileW(path, 0x80, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero)) {
                if (handle.IsInvalid) { error = Marshal.GetLastWin32Error(); return null; }
                FileInformation info;
                if (!GetFileInformationByHandle(handle, out info)) throw new Win32Exception(Marshal.GetLastWin32Error());
                if ((info.Attributes & 0x10) == 0) throw new IOException("Expected an existing directory.");
                var result = new StringBuilder(512);
                // FILE_NAME_NORMALIZED | VOLUME_NAME_GUID: drive aliases are eliminated.
                uint length = GetFinalPathNameByHandleW(handle, result, (uint)result.Capacity, 1);
                if (length == 0) throw new Win32Exception(Marshal.GetLastWin32Error());
                if (length >= result.Capacity) {
                    result = new StringBuilder(checked((int)length + 1));
                    length = GetFinalPathNameByHandleW(handle, result, (uint)result.Capacity, 1);
                    if (length == 0 || length >= result.Capacity) throw new IOException("Final directory path could not be resolved.");
                }
                string final = result.ToString();
                if (!final.StartsWith(@"\\?\Volume{", StringComparison.OrdinalIgnoreCase))
                    throw new IOException("A volume GUID path is required for output isolation.");
                error = 0;
                return final.TrimEnd('\\');
            }
        }

        public static string ExistingDirectory(string path) {
            int error;
            string result = TryDirectory(path, out error);
            if (result == null) throw new Win32Exception(error);
            return result;
        }

        public static string ProspectiveDirectory(string path) {
            var missing = new List<string>();
            string current = Path.GetFullPath(path);
            for (;;) {
                int error;
                string resolved = TryDirectory(current, out error);
                if (resolved != null) {
                    for (int index = missing.Count - 1; index >= 0; --index) resolved += @"\" + missing[index];
                    return resolved;
                }
                // Only a missing leaf/parent permits fallback; access or filesystem
                // failures must never turn an unresolved root into an allowed path.
                if (error != 2 && error != 3) throw new Win32Exception(error);
                string segment = Path.GetFileName(current.TrimEnd('\\')).TrimEnd(' ', '.');
                string parent = Path.GetDirectoryName(current.TrimEnd('\\'));
                if (String.IsNullOrEmpty(segment) || String.IsNullOrEmpty(parent) || parent == current)
                    throw new IOException("Product root ancestor could not be resolved.");
                missing.Add(segment);
                current = parent;
            }
        }
    }
}
'@
}

function Assert-A0BenchmarkOutputPath {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$ProductRoot)
    # Validate the submitted leaf before GetFullPath strips trailing dots/spaces.
    $leaf = [IO.Path]::GetFileName($Path)
    if ([string]::IsNullOrWhiteSpace($leaf) -or $leaf.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
        $leaf.EndsWith('.') -or $leaf.EndsWith(' ') -or
        $leaf -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
        throw 'Benchmark output must have a regular file name.'
    }
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetFullPath($ProductRoot).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    if ($full.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Benchmark output must be outside the product root.'
    }
    if (Test-Path -LiteralPath $full) { throw 'Benchmark output already exists.' }
    # A dangling leaf symlink also occupies the output name.
    if ($null -ne (Get-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue)) { throw 'Benchmark output name is occupied.' }
    Initialize-A0BenchmarkNativePaths
    $resolvedParent = [A0.M2Benchmark.NativePaths]::ExistingDirectory([IO.Path]::GetDirectoryName($full))
    $resolvedRoot = [A0.M2Benchmark.NativePaths]::ProspectiveDirectory($root)
    if ($resolvedParent.Equals($resolvedRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $resolvedParent.StartsWith($resolvedRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Resolved benchmark output must be outside the product root.'
    }
    # Check the operator-selected spelling too: final-path normalization follows
    # junctions, so checking only the final spelling would hide their presence.
    $parent = [IO.DirectoryInfo]::new([IO.Path]::GetDirectoryName($full))
    while ($null -ne $parent) {
        if (($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Reparse output parent is forbidden.' }
        $parent = $parent.Parent
    }
    return $full
}

Export-ModuleMember -Function Get-A0R7Percentile, Get-A0MetricMaximum, ConvertTo-A0RoundedMetric, New-A0ResourceMetricPair, Assert-A0BenchmarkRun, New-A0BenchmarkMetrics, Assert-A0BenchmarkOutputPath
