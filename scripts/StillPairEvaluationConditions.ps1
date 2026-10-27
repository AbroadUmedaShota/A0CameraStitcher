# Data-only persistence helpers, dot-sourced by Invoke-StillPairCorrection.
# Uses that command's pinned file/directory lifetime and error boundary.
function Get-ConditionFields($Element,[string[]]$Names) {
    if($Element.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) {throw 'Condition object required'}
    $seen=[System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($property in $Element.EnumerateObject()) {
        if($property.Name -cnotin $Names -or -not $seen.Add($property.Name)) {throw 'Unknown or duplicate condition member'}
    }
    if($seen.Count -ne $Names.Count) {throw 'Missing condition member'}
}
function Get-ConditionString($Element,[string]$Name) {
    $value=$Element.GetProperty($Name)
    if($value.ValueKind -ne [System.Text.Json.JsonValueKind]::String) {throw 'Condition string required'}
    return $value.GetString()
}
function Get-ConditionHash($Element,[string]$Name) {
    $value=Get-ConditionString $Element $Name
    if($value -cnotmatch '\A[0-9a-f]{64}\z') {throw 'Invalid condition digest'}
    return $value
}
function Get-ConditionInteger($Element,[string]$Name,[uint64]$Maximum) {
    $value=$Element.GetProperty($Name);[uint64]$number=0
    if($value.ValueKind -ne [System.Text.Json.JsonValueKind]::Number -or
        $value.GetRawText() -cnotmatch '^(0|[1-9][0-9]*)$' -or -not $value.TryGetUInt64([ref]$number) -or $number -gt $Maximum) {throw 'Invalid condition integer'}
    return $number
}
function Resolve-ConditionReference([string]$Root,[string]$Relative) {
    # Stored paths have exactly one spelling: forward-separated components,
    # no parent/current components, streams, drive/UNC or Windows aliases.
    if(-not $Relative -or $Relative.Contains('\')) {throw 'Invalid condition reference'}
    foreach($part in $Relative.Split('/')) {
        if(-not $part -or $part -in @('.','..') -or $part.EndsWith('.') -or $part.EndsWith(' ') -or
            $part.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
            $part -match '^(CON|PRN|AUX|NUL|COM[1-9\u00b9\u00b2\u00b3]|LPT[1-9\u00b9\u00b2\u00b3])(?:\.|$)') {throw 'Invalid condition reference component'}
    }
    $prefix=(Local-Path $Root).TrimEnd('\','/')+'\'
    $path=Local-Path ([IO.Path]::Combine($prefix,$Relative))
    if(-not $path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) {throw 'Condition reference escapes root'}
    return $path
}
function New-ConditionReference($File,[string]$Root) {
    $relative=[IO.Path]::GetRelativePath((Local-Path $Root),$File.Path).Replace('\','/')
    if((Resolve-ConditionReference $Root $relative) -ine $File.Path) {throw 'Condition reference mismatch'}
    return [ordered]@{relativePath=$relative;sha256=$File.sha256;encodedSizeBytes=[uint64]$File.encodedSizeBytes}
}
function Read-ConditionReference($Element,[uint64]$Maximum) {
    Get-ConditionFields $Element @('relativePath','sha256','encodedSizeBytes')
    $size=Get-ConditionInteger $Element 'encodedSizeBytes' $Maximum
    if($size -eq 0) {throw 'Empty condition source'}
    $path=Get-ConditionString $Element 'relativePath'
    $null=Resolve-ConditionReference $ConditionsRoot $path
    return [ordered]@{relativePath=$path;sha256=(Get-ConditionHash $Element 'sha256');encodedSizeBytes=$size}
}
function Assert-ConditionRaster($Raster) {
    if($Raster.rightUm -le $Raster.leftUm -or $Raster.bottomUm -le $Raster.topUm -or $Raster.dpi -eq 0 -or
        $Raster.widthPixels -eq 0 -or $Raster.heightPixels -eq 0 -or $Raster.resampling -cnotin @('bilinear','bicubic-catmull-rom')) {throw 'Invalid condition raster'}
    $width=[decimal]::Ceiling((([decimal]$Raster.rightUm-[decimal]$Raster.leftUm)*[decimal]$Raster.dpi)/25400)
    $height=[decimal]::Ceiling((([decimal]$Raster.bottomUm-[decimal]$Raster.topUm)*[decimal]$Raster.dpi)/25400)
    if($width -ne $Raster.widthPixels -or $height -ne $Raster.heightPixels) {throw 'Condition raster dimensions mismatch'}
}
function Read-ConditionSet($File) {
    $File.Stream.Position=0
    $reader=[IO.StreamReader]::new($File.Stream,[Text.UTF8Encoding]::new($false,$true),$false,4096,$true)
    try {$text=$reader.ReadToEnd()} finally {$reader.Dispose();$File.Stream.Position=0}
    $document=[System.Text.Json.JsonDocument]::Parse($text)
    try {
        $root=$document.RootElement
        Get-ConditionFields $root @('schemaVersion','productApproved','quality','pairId','reviewReportSha256','reviewImageSha256','inputs','profile','correction','color','raster')
        if((Get-ConditionString $root 'schemaVersion') -cne 'a0.saved-review-evaluation-conditions.v1' -or
            $root.GetProperty('productApproved').ValueKind -ne [System.Text.Json.JsonValueKind]::False -or
            (Get-ConditionString $root 'quality') -cne 'not-evaluated') {throw 'Unapproved condition schema required'}
        $inputArray=$root.GetProperty('inputs')
        if($inputArray.ValueKind -ne [System.Text.Json.JsonValueKind]::Array -or $inputArray.GetArrayLength() -ne 2) {throw 'Condition pair required'}
        $inputs=@(foreach($inputItem in $inputArray.EnumerateArray()) {
            Get-ConditionFields $inputItem @('cameraAlias','sha256','encodedSizeBytes')
            $size=Get-ConditionInteger $inputItem 'encodedSizeBytes' (64MB)
            if($size -eq 0) {throw 'Empty condition original'}
            [ordered]@{cameraAlias=(Get-ConditionString $inputItem 'cameraAlias');sha256=(Get-ConditionHash $inputItem 'sha256');encodedSizeBytes=$size}
        })
        if($inputs[0].cameraAlias -cne 'CAM-A' -or $inputs[1].cameraAlias -cne 'CAM-B') {throw 'Condition aliases mismatch'}
        $rasterElement=$root.GetProperty('raster')
        Get-ConditionFields $rasterElement @('leftUm','topUm','rightUm','bottomUm','dpi','widthPixels','heightPixels','resampling')
        $raster=[ordered]@{}
        foreach($name in @('leftUm','topUm','rightUm','bottomUm')) {$raster[$name]=Get-ConditionInteger $rasterElement $name ([uint64]::MaxValue)}
        foreach($name in @('dpi','widthPixels','heightPixels')) {$raster[$name]=Get-ConditionInteger $rasterElement $name ([uint32]::MaxValue)}
        $raster.resampling=Get-ConditionString $rasterElement 'resampling'
        Assert-ConditionRaster $raster
        $color=$null
        if($root.GetProperty('color').ValueKind -ne [System.Text.Json.JsonValueKind]::Null) {$color=Read-ConditionReference ($root.GetProperty('color')) (64KB)}
        return [ordered]@{pairId=(Get-ConditionString $root 'pairId');reviewReportSha256=(Get-ConditionHash $root 'reviewReportSha256');
            reviewImageSha256=(Get-ConditionHash $root 'reviewImageSha256');inputs=$inputs;
            profile=(Read-ConditionReference ($root.GetProperty('profile')) (256KB));
            correction=(Read-ConditionReference ($root.GetProperty('correction')) (64KB));color=$color;raster=$raster}
    } finally {$document.Dispose()}
}
function Write-PinnedJson([string]$Destination,$Record) {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Record|ConvertTo-Json -Depth 12 -Compress))
    $handle=[A0SavedReviewEvaluationFiles]::Create($Destination+'.partial');$writer=$null
    try {
        $writer=[IO.FileStream]::new($handle,[IO.FileAccess]::ReadWrite)
        $writer.Write($bytes,0,$bytes.Length);$writer.Flush($true)
        [A0SavedReviewEvaluationFiles]::Publish($handle,$Destination)
        $writer.Position=0;$copy=[byte[]]::new($bytes.Length);$offset=0
        while($offset -lt $copy.Length) {$count=$writer.Read($copy,$offset,$copy.Length-$offset);if($count -eq 0){throw 'Conditions reread incomplete'};$offset+=$count}
        if([Convert]::ToBase64String($bytes) -cne [Convert]::ToBase64String($copy)) {throw 'Conditions reread failed'}
    } finally {if($writer){$writer.Dispose()}else{$handle.Dispose()}}
}
