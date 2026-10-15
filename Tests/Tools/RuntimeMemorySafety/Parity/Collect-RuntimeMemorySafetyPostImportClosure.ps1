<#
Supplemental, read-only post-close asset observation. It never imports, copies
project data, launches an editor, or changes frozen timing evidence. OutputPath
must be fresh and outside both workroots. ReferenceOnly inventories all18;
otherwise all9 candidate C lanes are compared with paired A/Aprime references.
This supplements timing evidence; post-close bytes do not prove an in-memory
resource's identity during the earlier measurement window.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$ReferenceWorkRoot,
	[string]$CandidateWorkRoot='',
	[Parameter(Mandatory)][string]$OutputPath,
	[switch]$ReferenceOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Require([bool]$Condition,[string]$Message){if(!$Condition){throw $Message}}
function Full([string]$Path){[IO.Path]::GetFullPath($Path)}
function Below([string]$Child,[string]$Root){$Child.StartsWith($Root.TrimEnd('/','\')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)}
function NoRedirect([string]$Path){
	$cursor=Full $Path
	while($cursor){
		if(Test-Path -LiteralPath $cursor){Require (!((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) ('Redirected evidence: '+$cursor)}
		$cursor=[IO.Path]::GetDirectoryName($cursor)
	}
}
function Pin([string]$Path){
	NoRedirect $Path
	$before=Get-Item -LiteralPath $Path
	Require (!$before.PSIsContainer) ('Expected regular file: '+$Path)
	$length=$before.Length;$time=$before.LastWriteTimeUtc.Ticks
	$hash=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
	$after=Get-Item -LiteralPath $Path
	Require ($length -eq $after.Length -and $time -eq $after.LastWriteTimeUtc.Ticks -and
		$hash -ceq (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()) ('File changed during observation: '+$Path)
	return [pscustomobject]@{path=(Full $Path);bytes=$length;sha256=$hash;lastWriteUtc=$after.LastWriteTimeUtc.ToString('o')}
}
function ReadJson([string]$Path){Get-Content -LiteralPath $Path -Raw|ConvertFrom-Json}
$cooked=@(
	'Build/Assets/.terra-animation-manifests/da35202f311bd5ea56e23beb0aafbd9a.json',
	'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_493d9bc3.teanim',
	'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_fad19fd5.teanim',
	'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_9beae902.teskel',
	'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_b7d2176c.teskel',
	'Build/Assets/Models/ImportedHero_Generated/ImportedHeroBody.temf',
	'Build/Assets/Models/ImportedHero_Generated/ImportedHeroMaterial.tmat'
)
$runtimePaths=@($cooked)+@('Assets/Scenes/Scene.tescene','ProjectSettings/RenderSettings.json')
function ObserveLane([string]$Root,[string]$Scale,[string]$Role,[int]$Ordinal){
	$directory=Join-Path $Root "$Scale-$Role$Ordinal"
	$project=Join-Path $directory 'fixture/project'
	$receiptPath=Join-Path $directory 'capture/capture-receipt.json'
	$lifecyclePath=Join-Path $directory 'lifecycle.json'
	$receiptPin=Pin $receiptPath;$lifecyclePin=Pin $lifecyclePath
	$receipt=ReadJson $receiptPath;$lifecycle=ReadJson $lifecyclePath
	Require ($receipt.status -ceq 'Captured' -and $receipt.schemaVersion -ceq 'runtime-memory-safety-timing-v1' -and
		$receipt.metadata.scale -ceq $Scale -and $receipt.metadata.role -ceq $Role -and $receipt.metadata.ordinal -eq $Ordinal) 'Timing lane identity/status differs.'
	Require ($lifecycle.normalClose -and $lifecycle.exitCode -eq 0 -and !$lifecycle.timedOut -and $lifecycle.watchdogExited) 'Require closed successful timing lane.'
	NoRedirect $project
	$found=@(Get-ChildItem -LiteralPath (Join-Path $project 'Build/Assets') -Recurse -File | ForEach-Object {
		[IO.Path]::GetRelativePath($project,$_.FullName).Replace('\','/')
	} | Sort-Object)
	Require (($found -join "`n") -ceq (($cooked|Sort-Object) -join "`n")) ('Unexpected actual cooked path set: '+$directory)
	Require (!(Test-Path -LiteralPath (Join-Path $project 'ProjectSettings/PhysicsSettings.json'))) 'Unexpected project physics settings.'
	$runtime=[ordered]@{}
	foreach($relative in $runtimePaths){$runtime[$relative]=Pin (Join-Path $project $relative)}
	$materializationPath=Join-Path $directory 'fixture/materialization.json'
	$materializationPin=Pin $materializationPath;$materialization=ReadJson $materializationPath
	Require ($materialization.kind -ceq 'RuntimeMemorySafetyValidationProject' -and $materialization.scale -ceq $Scale -and $materialization.seed -eq 66007) 'Materialization identity differs.'
	Require ($runtime['Assets/Scenes/Scene.tescene'].sha256 -ceq $materialization.generatedHashes.'Assets/Scenes/Scene.tescene') 'Actual scene changed from its generated fixture.'
	# Other authored/importer metadata are retained and compared separately. They
	# cannot substitute for the compiled bytes actually admitted by this check.
	$authored=[ordered]@{}
	foreach($file in Get-ChildItem -LiteralPath (Join-Path $project 'Assets') -Recurse -File | Sort-Object FullName){
		$relative=[IO.Path]::GetRelativePath($project,$file.FullName).Replace('\','/')
		if($relative -cne 'Assets/Scenes/Scene.tescene'){$authored[$relative]=Pin $file.FullName}
	}
	$sourceDifferences=@(foreach($row in $materialization.inputClosure){
		$observed=Pin (Join-Path $project $row.projectPath)
		if($observed.sha256 -cne $row.sha256){[pscustomobject]@{relative=$row.projectPath;initialSha256=$row.sha256;actual=$observed}}
	})
	return [pscustomobject]@{lane="$Scale-$Role$Ordinal";scale=$Scale;role=$Role;ordinal=$Ordinal;
		capture=$receiptPin;lifecycle=$lifecyclePin;materialization=$materializationPin;
		createdUtc=$receipt.createdUtc;completedUtc=$receipt.completedUtc;runtime=$runtime;authored=$authored;initialClosureDifferences=$sourceDifferences}
}
function Differences($Expected,$Actual){
	$paths=@(@($Expected.Keys)+@($Actual.Keys)|Sort-Object -Unique)
	return @(foreach($relative in $paths){
		if(!$Expected.Contains($relative) -or !$Actual.Contains($relative)){
			[pscustomobject]@{relative=$relative;kind='PathSetDiffers'}
		}elseif($Expected[$relative].sha256 -cne $Actual[$relative].sha256 -or $Expected[$relative].bytes -ne $Actual[$relative].bytes){
			[pscustomobject]@{relative=$relative;kind='BytesDiffer';expected=$Expected[$relative];actual=$Actual[$relative]}
		}
	})
}
$result=[ordered]@{schemaVersion='runtime-memory-safety-post-import-closure-v1';status='Failed';observedUtc=[DateTime]::UtcNow.ToString('o');
	proof='Read-only filesystem bytes observed after successful normal process closure; not an in-window or in-memory resource receipt';
	nativeAcceptance=$false;performanceAcceptance=$false;timedInMemoryClosureVerified=$false}
$output='';$canWrite=$false
try{
	$reference=Full $ReferenceWorkRoot;$output=Full $OutputPath
	Require ([IO.Path]::IsPathRooted($ReferenceWorkRoot) -and [IO.Path]::IsPathRooted($OutputPath)) 'Use absolute paths.'
	Require (!(Test-Path -LiteralPath $output) -and (Test-Path -LiteralPath (Split-Path $output) -PathType Container)) 'Output must be fresh in an existing directory.'
	Require ($output -ine $reference -and !(Below $output $reference)) 'Output overlaps reference evidence.'
	NoRedirect $reference;NoRedirect $output
	if(!$ReferenceOnly){
		Require (![string]::IsNullOrWhiteSpace($CandidateWorkRoot) -and [IO.Path]::IsPathRooted($CandidateWorkRoot)) 'Require absolute CandidateWorkRoot or explicit ReferenceOnly.'
		$candidate=Full $CandidateWorkRoot;NoRedirect $candidate
		Require ($candidate -ine $reference -and !(Below $candidate $reference) -and !(Below $reference $candidate)) 'Candidate/reference roots overlap.'
		Require ($output -ine $candidate -and !(Below $output $candidate)) 'Output overlaps candidate evidence.'
	}else{Require ([string]::IsNullOrEmpty($CandidateWorkRoot)) 'ReferenceOnly cannot accept candidate captures.'}
	# Even an error receipt may be written only after every source root has been
	# excluded. Invalid output arguments must never create a file in evidence.
	$canWrite=$true
	$result.collector=Pin $PSCommandPath
	$references=@(foreach($scale in @('small','normal','stress')){foreach($ordinal in 1..3){foreach($role in @('A','Aprime')){ObserveLane $reference $scale $role $ordinal}}})
	$result.references=$references
	$common=$references[0].runtime
	foreach($lane in $references){
		foreach($relative in @($cooked)+@('ProjectSettings/RenderSettings.json')){
			Require ($lane.runtime[$relative].sha256 -ceq $common[$relative].sha256 -and $lane.runtime[$relative].bytes -eq $common[$relative].bytes) ('Reference actual runtime bytes differ: '+$lane.lane+' '+$relative)
		}
		$scaleFirst=@($references|Where-Object scale -CEQ $lane.scale)[0]
		Require ($lane.runtime['Assets/Scenes/Scene.tescene'].sha256 -ceq $scaleFirst.runtime['Assets/Scenes/Scene.tescene'].sha256) 'Reference scene differs within scale.'
	}
	$result.referenceCookedAndSettingsIdenticalAcross18=$true
	$result.referenceSceneIdenticalWithinEachScale=$true
	if($ReferenceOnly){
		Require ([string]::IsNullOrEmpty($CandidateWorkRoot)) 'ReferenceOnly cannot accept candidate captures.'
		$result.status='ReferenceObserved'
	}else{
		Require (![string]::IsNullOrWhiteSpace($CandidateWorkRoot) -and [IO.Path]::IsPathRooted($CandidateWorkRoot)) 'Require absolute CandidateWorkRoot or explicit ReferenceOnly.'
		$candidate=Full $CandidateWorkRoot;NoRedirect $candidate
		Require ($candidate -ine $reference -and !(Below $candidate $reference) -and !(Below $reference $candidate)) 'Candidate/reference roots overlap.'
		Require ($output -ine $candidate -and !(Below $output $candidate)) 'Output overlaps candidate evidence.'
		$candidates=@(foreach($scale in @('small','normal','stress')){foreach($ordinal in 1..3){ObserveLane $candidate $scale 'C' $ordinal}})
		$result.candidates=$candidates
		$result.pairs=@(foreach($lane in $candidates){foreach($role in @('A','Aprime')){
			$paired=@($references|Where-Object {$_.scale -ceq $lane.scale -and $_.ordinal -eq $lane.ordinal -and $_.role -ceq $role})
			Require ($paired.Count -eq 1) 'Ambiguous paired reference.'
			[pscustomobject]@{candidate=$lane.lane;reference=$paired[0].lane;runtimeDifferences=@(Differences $paired[0].runtime $lane.runtime);
				authoredImporterDifferences=@(Differences $paired[0].authored $lane.authored)}
		}})
		Require (@($result.pairs|Where-Object {$_.runtimeDifferences.Count -ne 0}).Count -eq 0) 'Candidate actual post-import runtime closure differs from paired reference.'
		$result.status='PostImportRuntimeClosureEqual';$result.postImportRuntimeClosureParity=$true
	}
	[IO.File]::WriteAllText($output,($result|ConvertTo-Json -Depth 80),[Text.UTF8Encoding]::new($false))
	exit 0
}catch{
	$result.reason=$_.Exception.Message
	if($canWrite -and $output -and !(Test-Path -LiteralPath $output)){
		try{[IO.File]::WriteAllText($output,($result|ConvertTo-Json -Depth 80),[Text.UTF8Encoding]::new($false))}catch{}
	}
	Write-Error $result.reason -ErrorAction Continue
	exit 2
}
