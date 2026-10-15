<#
Read-only source provenance for the matching-ABI parity runner. Prerequisites:
PowerShell 7, the existing Git executable, a checkout containing the archived
revision, the immutable reference receipt and its Compatibility directory.
Only a fresh directory beneath RepositoryRoot/out is created. No runtime runs.
Usage: & ./New-RuntimeMemorySafetyRendererSourceProof.ps1 -RepositoryRoot <absolute checkout>
 -ReferenceReceipt <absolute reference.json> -ReferenceCompatibilityRoot <absolute Compatibility>
 -GitPath <absolute git executable> -OutputRoot <fresh absolute out directory>
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$RepositoryRoot,
	[Parameter(Mandatory)][string]$ReferenceReceipt,
	[Parameter(Mandatory)][string]$ReferenceCompatibilityRoot,
	[Parameter(Mandatory)][string]$GitPath,
	[Parameter(Mandatory)][string]$OutputRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Require([bool]$Condition,[string]$Message){if(!$Condition){throw $Message}}
function Full([string]$Path){[IO.Path]::GetFullPath($Path)}
function Below([string]$Child,[string]$Root){$Child.StartsWith($Root.TrimEnd('/','\')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)}
function NoRedirect([string]$Path){$cursor=Full $Path;while($cursor){if(Test-Path -LiteralPath $cursor){Require (!((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) ('Redirected source proof path: '+$cursor)};$cursor=[IO.Path]::GetDirectoryName($cursor)}}
function Pin([string]$Path){
	NoRedirect $Path;$before=Get-Item -LiteralPath $Path
	Require (!$before.PSIsContainer) 'Source proof pin requires a regular file.'
	$length=$before.Length;$ticks=$before.LastWriteTimeUtc.Ticks;$hash=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();$after=Get-Item -LiteralPath $Path
	Require ($length -eq $after.Length -and $ticks -eq $after.LastWriteTimeUtc.Ticks) ('Source changed while hashing: '+$Path)
	return [pscustomobject]@{path=(Full $Path);bytes=$length;sha256=$hash}
}
function Verify($Value){$actual=Pin $Value.path;Require ($actual.bytes -eq $Value.bytes -and $actual.sha256 -ceq $Value.sha256) ('Source input changed: '+$Value.path)}
function GitBytes([string[]]$Arguments){
	# Read the process pipe as bytes: PowerShell text redirection can change old
	# Git blobs' line endings/encoding and invalidate their content identities.
	$start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$git;$start.UseShellExecute=$false;$start.CreateNoWindow=$true
	$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
	foreach($argument in @('--no-pager','-C',$repo)+$Arguments){$start.ArgumentList.Add($argument)}
	$process=[Diagnostics.Process]::new();$process.StartInfo=$start;$memory=[IO.MemoryStream]::new()
	try{
		Require ($process.Start()) 'Could not start the supplied Git executable.'
		$stderr=$process.StandardError.ReadToEndAsync();$process.StandardOutput.BaseStream.CopyTo($memory);$process.WaitForExit()
		$errorText=$stderr.GetAwaiter().GetResult();Require ($process.ExitCode -eq 0) ('Read-only Git query failed: '+$errorText)
		return ,$memory.ToArray()
	}finally{$memory.Dispose();$process.Dispose()}
}
function Pair([string]$RelativePath){
	$bytes=GitBytes @('show',($revision+':'+$RelativePath))
	$blob=[Text.Encoding]::UTF8.GetString((GitBytes @('rev-parse',($revision+':'+$RelativePath)))).Trim()
	Require ($blob -cmatch '^[0-9a-f]{40}$') 'Reference source does not resolve to a Git blob.'
	$prefix=[Text.Encoding]::UTF8.GetBytes('blob '+$bytes.Length+[char]0)
	$actual=[Convert]::ToHexString([Security.Cryptography.SHA1]::HashData([byte[]]($prefix+$bytes))).ToLowerInvariant()
	Require ($actual -ceq $blob) 'Archived source bytes do not match the actual Git blob.'
	$destination=Join-Path $output ('reference-'+[IO.Path]::GetFileName($RelativePath))
	Require (!(Test-Path -LiteralPath $destination)) 'Archived source output must be fresh.'
	[IO.File]::WriteAllBytes($destination,$bytes)
	return [pscustomobject]@{sourcePath=$RelativePath;referenceGitBlob=$blob;referenceSource=(Pin $destination);candidateSource=(Pin (Join-Path $repo $RelativePath))}
}
foreach($path in @($RepositoryRoot,$ReferenceReceipt,$ReferenceCompatibilityRoot,$GitPath,$OutputRoot)){Require ([IO.Path]::IsPathRooted($path)) 'All source proof paths must be absolute.';NoRedirect $path}
$repo=Full $RepositoryRoot;$reference=Full $ReferenceReceipt;$compatibility=Full $ReferenceCompatibilityRoot;$git=Full $GitPath;$output=Full $OutputRoot
Require ((Test-Path -LiteralPath (Join-Path $repo '.git')) -and (Test-Path -LiteralPath $compatibility -PathType Container)) 'Missing checkout or archived Compatibility directory.'
Require (!(Test-Path -LiteralPath $output) -and (Below $output (Join-Path $repo 'out'))) 'OutputRoot must be a fresh directory beneath checkout/out.'
foreach($protected in @($compatibility,(Split-Path $reference),$PSScriptRoot,(Join-Path $repo 'out/reference'))){
	$protected=Full $protected;Require ($output -ine $protected -and !(Below $output $protected) -and !(Below $protected $output)) 'Source proof output overlaps protected inputs.'
}
$referencePin=Pin $reference;$gitPin=Pin $git;$generatorPin=Pin $PSCommandPath
$receipt=Get-Content -LiteralPath $reference -Raw|ConvertFrom-Json
$revision=[string]$receipt.sourceRevision;Require ($revision -cmatch '^[0-9a-f]{40}$') 'Reference receipt requires an exact source revision.'
$resolved=[Text.Encoding]::UTF8.GetString((GitBytes @('rev-parse',($revision+'^{commit}')))).Trim();Require ($resolved -ceq $revision) 'Reference revision does not resolve exactly.'
[IO.Directory]::CreateDirectory($output)|Out-Null
$normal=Pair 'Engine/Renderer/RenderSystem.cpp'
$additional=@(foreach($name in @('VulkanShadowValidationReadback.cpp','VulkanReadback.cpp','VulkanEditorRendering.cpp','VulkanContextImpl.h','VulkanFrameRendering.cpp')){Pair ('Engine/Renderer/RHI/Vulkan/'+$name)})
$capacityPattern='static\s+constexpr\s+uint32_t\s+MAX_RENDER_SNAPSHOTS_PER_FRAME\s*=\s*8u\s*;'
$compatPin=Pin (Join-Path $compatibility 'Engine/Renderer/RHI/Vulkan/VulkanContextImpl.h')
foreach($pair in @($normal)+$additional){
	foreach($side in @('referenceSource','candidateSource')){
		$text=[IO.File]::ReadAllText($pair.$side.path)
		if($pair.sourcePath -ceq 'Engine/Renderer/RenderSystem.cpp'){
			Require ([regex]::IsMatch($text,'for\s*\(\s*size_t\s+cameraIndex\s*=\s*0;\s*cameraIndex\s*<\s*cameras\.size\(\)') -and $text.Contains('cameras.front().entity') -and $text.Contains('RenderOutputKind::MainRuntimeView') -and $text.Contains('queuePreparedSubmission')) 'Normal authored-camera attempt loop or shared output policy differs.'
		}
		if($pair.sourcePath.EndsWith('/VulkanContextImpl.h')){Require ([regex]::IsMatch($text,$capacityPattern)) 'Old/current concurrent snapshot capacity differs.'}
		if($pair.sourcePath.EndsWith('/VulkanFrameRendering.cpp')){
			foreach($expression in @('if\s*\(\s*!impl->editorMode\s*\)','(?:vkQueueSubmit|SubmitGraphicsQueue)\s*\(','(?:vkQueueWaitIdle|WaitGraphicsQueueIdle)\s*\(','renderSnapshotSlotsUsed\s*\[\s*currentFrame\s*\]\.fill\(false\)','RenderSnapshotFallbackResult::SerializedSafely')){Require ([regex]::IsMatch($text,$expression)) 'Inherited editor prefix submit/wait/reuse source policy differs.'}
		}
		if($pair.sourcePath.EndsWith('/VulkanReadback.cpp')){Require ($text.Contains('!impl->editorMode && !captureDedicatedView') -and $text.Contains('impl->gameView')) 'Normal editor shared target readback path differs.'}
		if($pair.sourcePath.EndsWith('/VulkanShadowValidationReadback.cpp')){Require ($text.Contains('Duplicate native lighting purpose observation.') -and $text.Contains('request.identity.frameSerial == identity.frameSerial') -and $text.Contains('request.identity.viewId == identity.viewId')) 'Native lighting deduplication source policy differs.'}
		if($pair.sourcePath.EndsWith('/VulkanEditorRendering.cpp')){Require ($text.Contains('if (renderUI)') -and $text.Contains('editorCompletionTracker.RecordSubmission')) 'Optional editor UI presentation/queue receipt source differs.'}
	}
}
$capacity=@($additional|Where-Object sourcePath -like '*/VulkanContextImpl.h')[0]
$compatText=[IO.File]::ReadAllText($compatPin.path).Replace("`r`n","`n")
Require ([regex]::IsMatch($compatText,$capacityPattern) -and $compatText -ceq [IO.File]::ReadAllText($capacity.referenceSource.path).Replace("`r`n","`n")) 'Archived Compatibility header differs beyond line endings from the reference Git source.'
$findings=ConvertFrom-Json -InputObject @'
[
  {
    "symbol": "RenderSystem::CaptureFrameSnapshot viewRequests",
    "fact": "MainRuntimeView logical request/reference owner uses cameras.front(), outputId2."
  },
  {
    "symbol": "RenderSystem::Update Submit Runtime Cameras",
    "fact": "Both source versions loop cameraIndex<cameras.size() and queue one MainRuntimeView submission per authored camera, all outputId2."
  },
  {
    "symbol": "VulkanShadowValidationReadback::RecordLighting",
    "fact": "Readback sampled ownership is keyed viewType/viewId; duplicate pending lighting requests reject same frame/view/purpose, not commandSerial. This can limit observable per-command evidence for multiple cameras sharing one output owner."
  },
  {
    "symbol": "VulkanContext::RenderFrame snapshot admission",
    "fact": "Both headers declare eight concurrent slices. Both full frame-rendering sources distinguish runtime skip from editor required-output prefix submit/wait/completion/slot reset/retry. The editor path can record all sixteen commands; overflow serialization must be observed, not inferred as GPU success."
  }
]
'@
$interpretation='Both versions attempt every authored camera in captured order into shared Game output2. Eight is concurrent physical snapshot capacity. Runtime required overflow skips further passes, but the approved editor path submits/waits/completes the first prefix, resets snapshot occupancy and continues all N commands. Full2N native metadata and actual same-frame final normal Game View are required. All actually available complete lighting artifact pairs are compared; deduplicated rows retain recording metadata without fabricated queue completion. Baseline private arrays and individual later-command image bytes remain unobserved.'
# Verify every mutable source again before sealing the copied provenance.
foreach($pair in @($normal)+$additional){Verify $pair.referenceSource;Verify $pair.candidateSource}
foreach($value in @($referencePin,$compatPin,$gitPin,$generatorPin)){Verify $value}
$result=[ordered]@{sourceRevision=$revision;referenceGitBlob=$normal.referenceGitBlob;referenceSource=$normal.referenceSource;candidateSource=$normal.candidateSource;
	findings=$findings;interpretation=$interpretation;additionalSources=$additional;referenceCompatibilityCapacityHeader=$compatPin;backendSnapshotCapacity=8;
	provenance=@{referenceReceipt=$referencePin;generator=$generatorPin;gitExecutable=$gitPin}}
$receiptPath=Join-Path $output 'source-proof.json';$stream=[IO.FileStream]::new($receiptPath,[IO.FileMode]::CreateNew)
try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($result|ConvertTo-Json -Depth 40));$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
Write-Output $receiptPath
exit 0
