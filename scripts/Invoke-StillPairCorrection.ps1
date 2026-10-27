#requires -Version 7.2
[CmdletBinding(DefaultParameterSetName='Explicit')]
param(
    [Parameter(Mandatory)][string]$Report,
    [Parameter(Mandatory)][string]$Inspector,
    [Parameter(Mandatory)][string]$Evaluator,
    [Parameter(Mandatory,ParameterSetName='Explicit')][string]$ProfileFile,
    [Parameter(Mandatory,ParameterSetName='Explicit')][string]$CorrectionJob,
    [Parameter(ParameterSetName='Explicit')][string]$ColorJob,
    [Parameter(Mandatory)][string]$ProductRoot,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory,ParameterSetName='Explicit')][uint64]$LeftUm,
    [Parameter(Mandatory,ParameterSetName='Explicit')][uint64]$TopUm,
    [Parameter(Mandatory,ParameterSetName='Explicit')][uint64]$RightUm,
    [Parameter(Mandatory,ParameterSetName='Explicit')][uint64]$BottomUm,
    [Parameter(Mandatory,ParameterSetName='Explicit')][uint32]$Dpi,
    [Parameter(Mandatory,ParameterSetName='Explicit')][uint32]$WidthPixels,
    [Parameter(Mandatory,ParameterSetName='Explicit')][uint32]$HeightPixels,
    [Parameter(Mandatory,ParameterSetName='Explicit')][ValidateSet('bilinear','bicubic-catmull-rom')][string]$Resampling,
    [Parameter(Mandatory,ParameterSetName='Saved')][string]$ConditionsFile,
    [Parameter(Mandatory,ParameterSetName='Saved')][Parameter(ParameterSetName='Explicit')][string]$ConditionsRoot,
    [Parameter(ParameterSetName='Explicit')][switch]$SaveConditions
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$phase='arguments'; $resultCode=2
$files=[System.Collections.Generic.List[object]]::new()
$directories=[System.Collections.Generic.List[object]]::new()
$pinned=[System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
try {
    # Directory pins and non-replacing, handle-based binding publication use
    # the same Windows ownership rules as the native evaluation boundary.
    if(-not ('A0SavedReviewEvaluationFiles' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class A0SavedReviewEvaluationFiles {
    [StructLayout(LayoutKind.Sequential)] struct Tag { public uint Attributes, ReparseTag; }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string name,uint access,uint share,IntPtr security,uint disposition,uint flags,IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool GetFileInformationByHandleEx(SafeFileHandle file,int kind,out Tag value,uint size);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool SetFileInformationByHandle(SafeFileHandle file,int kind,IntPtr data,uint size);
    public static SafeFileHandle Pin(string path) {
        var h=CreateFileW(path,0x80,1,IntPtr.Zero,3,0x02200000,IntPtr.Zero);
        Tag tag;
        if(h.IsInvalid || !GetFileInformationByHandleEx(h,9,out tag,8) || (tag.Attributes&0x410)!=0x10) {
            h.Dispose(); throw new InvalidOperationException("Ordinary directory pin failed");
        }
        return h;
    }
    public static SafeFileHandle Create(string path) {
        var h=CreateFileW(path,0xC0010000,1,IntPtr.Zero,1,0x00200000,IntPtr.Zero);
        if(h.IsInvalid) {h.Dispose(); throw new InvalidOperationException("Binding reservation failed");}
        return h;
    }
    public static void Publish(SafeFileHandle file,string destination) {
        byte[] name=System.Text.Encoding.Unicode.GetBytes(destination);
        int root=IntPtr.Size==8?8:4, length=root+IntPtr.Size, start=length+4;
        int size=start+name.Length+4;
        IntPtr data=Marshal.AllocHGlobal(size);
        try {
            for(int i=0;i<size;i++) Marshal.WriteByte(data,i,0);
            Marshal.WriteInt32(data,length,name.Length); Marshal.Copy(name,0,IntPtr.Add(data,start),name.Length);
            if(!SetFileInformationByHandle(file,3,data,(uint)size))
                throw new Win32Exception(Marshal.GetLastWin32Error());
        } finally {Marshal.FreeHGlobal(data);}
    }
}
'@
    }
    function Local-Path([string]$Value) {
        if($Value -notmatch '^[A-Za-z]:[\\/]' -or $Value.Substring(2).Contains(':')) {throw 'Ordinary local path required'}
        foreach($part in ($Value.Substring(3) -split '[\\/]')) {
            if($part -in @('.','..') -or $part.EndsWith('.') -or $part.EndsWith(' ')) {throw 'Ambiguous path'}
        }
        $full=[IO.Path]::GetFullPath($Value)
        if([IO.DriveInfo]::new([IO.Path]::GetPathRoot($full)).DriveType -ne [IO.DriveType]::Fixed) {throw 'Fixed drive required'}
        return $full
    }
    function Pin-Directory([string]$Path) {
        $chain=[System.Collections.Generic.Stack[string]]::new()
        for($dir=[IO.DirectoryInfo]::new($Path);$null -ne $dir;$dir=$dir.Parent) {$chain.Push($dir.FullName)}
        while($chain.Count) {
            $item=$chain.Pop()
            if($pinned.Add($item)) {$directories.Add([A0SavedReviewEvaluationFiles]::Pin($item))}
        }
    }
    function Stream-Hash($Stream) {
        $Stream.Position=0; $sha=[Security.Cryptography.SHA256]::Create()
        try {return [Convert]::ToHexString($sha.ComputeHash($Stream)).ToLowerInvariant()}
        finally {$sha.Dispose();$Stream.Position=0}
    }
    function Pin-File([string]$Path,[uint64]$Maximum,[string]$ExpectedHash='',[uint64]$ExpectedSize=0) {
        $full=Local-Path $Path; Pin-Directory ([IO.Path]::GetDirectoryName($full))
        if(([IO.File]::GetAttributes($full) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {throw 'Reparse file'}
        $stream=[IO.File]::Open($full,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        $item=[pscustomobject]@{Stream=$stream;Path=$full;sha256='';encodedSizeBytes=$stream.Length}
        $files.Add($item)
        if($stream.Length -le 0 -or [uint64]$stream.Length -gt $Maximum) {throw 'Input size limit'}
        $item.sha256=Stream-Hash $stream
        if($ExpectedHash -and ($item.sha256 -cne $ExpectedHash -or $item.encodedSizeBytes -ne $ExpectedSize)) {throw 'Snapshot mismatch'}
        return $item
    }
    function Invoke-Tool([string]$Exe,[string[]]$Arguments) {
        $start=[Diagnostics.ProcessStartInfo]::new($Exe)
        $start.UseShellExecute=$false; $start.CreateNoWindow=$true
        $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
        $start.StandardOutputEncoding=[Text.UTF8Encoding]::new($false,$true)
        $start.StandardErrorEncoding=[Text.UTF8Encoding]::new($false,$true)
        foreach($argument in $Arguments) {$start.ArgumentList.Add($argument)}
        $child=[Diagnostics.Process]::new(); $child.StartInfo=$start
        try {
            if(-not $child.Start()) {throw 'Child did not start'}
            $stdout=$child.StandardOutput.ReadToEndAsync(); $stderr=$child.StandardError.ReadToEndAsync()
            $child.WaitForExit()
            return [pscustomobject]@{Code=$child.ExitCode;Out=$stdout.GetAwaiter().GetResult();Error=$stderr.GetAwaiter().GetResult()}
        } finally {$child.Dispose()}
    }
    function Read-Json($Item) {
        $Item.Stream.Position=0
        $reader=[IO.StreamReader]::new($Item.Stream,[Text.UTF8Encoding]::new($false,$true),$true,4096,$true)
        try {return ($reader.ReadToEnd()|ConvertFrom-Json -AsHashtable -Depth 64)}
        finally {$reader.Dispose();$Item.Stream.Position=0}
    }
    function Same-Inputs($Items,$Expected) {
        if(@($Items).Count -ne 2) {throw 'Pair count mismatch'}
        for($i=0;$i -lt 2;$i++) {
            if($Items[$i].cameraAlias -cne $Expected[$i].cameraAlias -or $Items[$i].sha256 -cne $Expected[$i].sha256 -or
                $Items[$i].encodedSizeBytes -ne $Expected[$i].encodedSizeBytes) {throw 'Pair binding mismatch'}
        }
    }
    . (Join-Path $PSScriptRoot 'StillPairEvaluationConditions.ps1')
    $conditionFile=$null;$conditionRecord=$null
    if($PSCmdlet.ParameterSetName -eq 'Saved' -or $SaveConditions) {
        if(-not $ConditionsRoot) {throw 'Explicit conditions root required'}
        $ConditionsRoot=Local-Path $ConditionsRoot;Pin-Directory $ConditionsRoot
    }
    if($PSCmdlet.ParameterSetName -eq 'Saved') {
        $phase='conditions'
        $conditionFile=Pin-File $ConditionsFile (64KB)
        $conditionRecord=Read-ConditionSet $conditionFile
        $ProfileFile=Resolve-ConditionReference $ConditionsRoot $conditionRecord.profile.relativePath
        $CorrectionJob=Resolve-ConditionReference $ConditionsRoot $conditionRecord.correction.relativePath
        $ColorJob=if($conditionRecord.color){Resolve-ConditionReference $ConditionsRoot $conditionRecord.color.relativePath}else{''}
        $LeftUm=$conditionRecord.raster.leftUm;$TopUm=$conditionRecord.raster.topUm
        $RightUm=$conditionRecord.raster.rightUm;$BottomUm=$conditionRecord.raster.bottomUm
        $Dpi=$conditionRecord.raster.dpi;$WidthPixels=$conditionRecord.raster.widthPixels;$HeightPixels=$conditionRecord.raster.heightPixels
        $Resampling=$conditionRecord.raster.resampling
    }
    $inspectorPath=Local-Path $Inspector; $evaluatorPath=Local-Path $Evaluator
    $reportPath=Local-Path $Report; $outputPath=Local-Path $OutputDirectory
    if(-not [IO.File]::Exists($inspectorPath) -or -not [IO.File]::Exists($evaluatorPath) -or [IO.File]::Exists($outputPath) -or [IO.Directory]::Exists($outputPath)) {throw 'Executable/output selection'}
    Pin-Directory ([IO.Path]::GetDirectoryName($outputPath))
    $phase='review'
    $selection=Invoke-Tool $inspectorPath @('--report',$reportPath,'--evaluation-inputs')
    if($selection.Code -ne 0 -or $selection.Error) {throw 'Review validation failed'}
    $selected=$selection.Out|ConvertFrom-Json -AsHashtable -Depth 32
    # mountOrientation is a record only: StitchEval reads the unrotated originals and
    # the draft profile projection absorbs the mounting, so no pixels are turned here.
    if($selected.schemaVersion -cne 'a0.saved-review-evaluation-inputs.v1' -or $selected.productApproved -ne $false -or $selected.quality -cne 'not-evaluated' -or
        $selected.mountOrientation -isnot [string] -or $selected.mountOrientation -cnotin @('normal','rotated-180')) {throw 'Review handoff contract'}
    foreach($inputFile in @($selected.reviewReport,$selected.reviewImage)+@($selected.inputs)+@($selected.parentReview)|Where-Object {$null -ne $_}) {
        $null=Pin-File $inputFile.path (64MB) $inputFile.sha256 $inputFile.encodedSizeBytes
    }
    $profile=Pin-File $ProfileFile (256KB); $geometry=Pin-File $CorrectionJob (64KB)
    $color=$null; if($ColorJob) {$color=Pin-File $ColorJob (64KB)}
    $raster=[ordered]@{leftUm=$LeftUm;topUm=$TopUm;rightUm=$RightUm;bottomUm=$BottomUm;dpi=$Dpi;widthPixels=$WidthPixels;heightPixels=$HeightPixels;resampling=$Resampling}
    if($SaveConditions -or $conditionRecord) {
        $phase='conditions';Assert-ConditionRaster $raster
        $profileReference=New-ConditionReference $profile $ConditionsRoot
        $geometryReference=New-ConditionReference $geometry $ConditionsRoot
        $colorReference=if($color){New-ConditionReference $color $ConditionsRoot}else{$null}
        if($conditionRecord) {
            if($conditionRecord.pairId -cne $selected.pairId -or $conditionRecord.reviewReportSha256 -cne $selected.reviewReport.sha256 -or
                $conditionRecord.reviewImageSha256 -cne $selected.reviewImage.sha256) {throw 'Condition review mismatch'}
            Same-Inputs $conditionRecord.inputs $selected.inputs
            foreach($name in @('profile','correction','color')) {
                $expected=$conditionRecord[$name];$actual=switch($name){'profile'{$profileReference};'correction'{$geometryReference};'color'{$colorReference}}
                if(($null -eq $expected) -ne ($null -eq $actual) -or ($expected -and ($expected.sha256 -cne $actual.sha256 -or $expected.encodedSizeBytes -ne $actual.encodedSizeBytes))) {throw 'Condition source mismatch'}
            }
        }
    }
    $again=Invoke-Tool $inspectorPath @('--report',$reportPath,'--evaluation-inputs')
    if($again.Code -ne 0 -or $again.Out -cne $selection.Out) {throw 'Selected review changed'}
    $phase='evaluator'
    $arguments=@('--camera-a',$selected.inputs[0].path,'--camera-b',$selected.inputs[1].path,
        '--profile-file',$profile.Path,'--product-root',(Local-Path $ProductRoot),'--output-directory',$outputPath,
        '--left-um',"$LeftUm",'--top-um',"$TopUm",'--right-um',"$RightUm",'--bottom-um',"$BottomUm",
        '--dpi',"$Dpi",'--width-pixels',"$WidthPixels",'--height-pixels',"$HeightPixels",'--resampling',$Resampling,
        '--correction-job',$geometry.Path)
    if($color) {$arguments+=@('--color-job',$color.Path)}
    $evaluated=Invoke-Tool $evaluatorPath $arguments
    if($evaluated.Code -notin @(0,3) -or $evaluated.Error) {throw 'Evaluator failed'}
    $out=$evaluated.Out.Replace("`r",'').TrimEnd("`n")
    $outcome=if($evaluated.Code -eq 0){'evaluated'}elseif($out.StartsWith('result=color-rejected')){'color-rejected'}else{'correction-rejected'}
    $lastLine=if($outcome -eq 'evaluated'){'manifest=stitch-eval.manifest.json'}elseif($outcome -eq 'color-rejected'){'report=color-report.json'}else{'report=correction-report.json'}
    if($out -cne "result=$outcome`nprofileStatus=draft`nquality=not-evaluated`n$lastLine") {throw 'Evaluator output contract'}
    $phase='binding'
    Pin-Directory $outputPath
    $artifacts=[System.Collections.Generic.List[object]]::new()
    function Artifact([string]$Name,[uint64]$Maximum) {
        $item=Pin-File (Join-Path $outputPath $Name) $Maximum
        $artifacts.Add([ordered]@{relativePath=$Name;sha256=$item.sha256;encodedSizeBytes=$item.encodedSizeBytes})
        return $item
    }
    $correction=Artifact 'correction-report.json' (32MB); $correctionRecord=Read-Json $correction
    Same-Inputs $correctionRecord.inputs $selected.inputs
    if($correctionRecord.profileFileSha256 -cne $profile.sha256 -or $correctionRecord.jobSha256 -cne $geometry.sha256 -or
        $correctionRecord.productApproved -ne $false -or $correctionRecord.quality -cne 'not-evaluated') {throw 'Geometry binding mismatch'}
    $colorReport=$null
    if($color -and $outcome -ne 'correction-rejected') {
        $colorReport=Artifact 'color-report.json' (32MB); $colorRecord=Read-Json $colorReport
        Same-Inputs $colorRecord.inputs $selected.inputs
        if($colorRecord.jobSha256 -cne $color.sha256 -or $colorRecord.profileFileSha256 -cne $profile.sha256 -or
            $colorRecord.geometryReportSha256 -cne $correction.sha256 -or $colorRecord.productApproved -ne $false) {throw 'Color binding mismatch'}
    }
    if($outcome -eq 'evaluated') {
        $image=Artifact 'evaluation.jpg' (64MB); $manifest=Artifact 'stitch-eval.manifest.json' (256KB); $record=Read-Json $manifest
        Same-Inputs $record.inputs $selected.inputs
        if($record.profileStatus -cne 'draft' -or $record.quality -cne 'not-evaluated' -or
            $record.output.sha256 -cne $image.sha256 -or $record.output.encodedSizeBytes -ne $image.encodedSizeBytes -or
            $record.correction.report.sha256 -cne $correction.sha256 -or $record.correction.jobSha256 -cne $geometry.sha256 -or
            $record.correction.profileFileSha256 -cne $profile.sha256) {throw 'Manifest binding mismatch'}
        if($color -and $record.colorCorrection.report.sha256 -cne $colorReport.sha256) {throw 'Color manifest mismatch'}
    } elseif([IO.File]::Exists((Join-Path $outputPath 'evaluation.jpg')) -or [IO.File]::Exists((Join-Path $outputPath 'stitch-eval.manifest.json'))) {throw 'Rejected evaluation published success'}
    if($SaveConditions) {
        $conditionSet=[ordered]@{schemaVersion='a0.saved-review-evaluation-conditions.v1';productApproved=$false;quality='not-evaluated';
            pairId=$selected.pairId;reviewReportSha256=$selected.reviewReport.sha256;reviewImageSha256=$selected.reviewImage.sha256;
            inputs=@($selected.inputs|ForEach-Object {[ordered]@{cameraAlias=$_.cameraAlias;sha256=$_.sha256;encodedSizeBytes=$_.encodedSizeBytes}});
            profile=$profileReference;correction=$geometryReference;color=$colorReference;raster=$raster}
        Write-PinnedJson (Join-Path $outputPath 'evaluation-conditions.json') $conditionSet
        $conditionFile=Artifact 'evaluation-conditions.json' (64KB)
    }
    $again=Invoke-Tool $inspectorPath @('--report',$reportPath,'--evaluation-inputs')
    if($again.Code -ne 0 -or $again.Out -cne $selection.Out) {throw 'Review changed during evaluation'}
    foreach($item in $files) {if($item.Stream.Length -ne $item.encodedSizeBytes -or (Stream-Hash $item.Stream) -cne $item.sha256) {throw 'Held snapshot changed'}}
    $binding=[ordered]@{
        schemaVersion='a0.saved-review-evaluation-binding.v1';result=$outcome;productApproved=$false;quality='not-evaluated'
        pairId=$selected.pairId;reviewReportSha256=$selected.reviewReport.sha256;reviewImageSha256=$selected.reviewImage.sha256
        inputs=@($selected.inputs|ForEach-Object {[ordered]@{cameraAlias=$_.cameraAlias;sha256=$_.sha256;encodedSizeBytes=$_.encodedSizeBytes}})
        profileFileSha256=$profile.sha256;correctionJobSha256=$geometry.sha256;colorJobSha256=$(if($color){$color.sha256}else{$null})
        artifacts=@($artifacts)
    }
    if($conditionFile) {
        $binding.schemaVersion='a0.saved-review-evaluation-binding.v2'
        $binding.conditionSet=[ordered]@{sha256=$conditionFile.sha256;encodedSizeBytes=$conditionFile.encodedSizeBytes}
    }
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($binding|ConvertTo-Json -Depth 12 -Compress))
    $destination=Join-Path $outputPath 'review-evaluation-binding.json'
    $handle=[A0SavedReviewEvaluationFiles]::Create($destination+'.partial')
    $writer=$null
    try {
        $writer=[IO.FileStream]::new($handle,[IO.FileAccess]::ReadWrite)
        $writer.Write($bytes,0,$bytes.Length);$writer.Flush($true)
        [A0SavedReviewEvaluationFiles]::Publish($handle,$destination)
        $writer.Position=0;$copy=[byte[]]::new($bytes.Length);$offset=0
        while($offset -lt $copy.Length) {
            $count=$writer.Read($copy,$offset,$copy.Length-$offset)
            if($count -eq 0) {throw 'Binding reread incomplete'}
            $offset+=$count
        }
        if([Convert]::ToBase64String($bytes) -cne [Convert]::ToBase64String($copy)) {throw 'Binding reread failed'}
    } finally {if($writer){$writer.Dispose()}else{$handle.Dispose()}}
    [Console]::Out.WriteLine("result=$outcome`nprofileStatus=draft`nquality=not-evaluated`nbinding=review-evaluation-binding.json")
    $resultCode=$evaluated.Code
} catch {
    [Console]::Error.WriteLine("error=review-evaluation-$phase-failed")
} finally {
    foreach($item in $files) {$item.Stream.Dispose()}
    for($i=$directories.Count-1;$i -ge 0;$i--) {$directories[$i].Dispose()}
}
exit $resultCode
