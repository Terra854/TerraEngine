<#
Matching-ABI parity orchestration after a completed candidate build/install.
Narrowly builds the two matching-ABI hosts, stages matching native/Managed files,
then materializes, captures under watchdog and compares each scale serially.
No source/reference/frozen-tool edits, imports, deletes or evidence replacement.
Exit0 means requested comparisons completed equal; acceptance remains parent-owned.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$CandidateInstallRoot,
	[Parameter(Mandatory)][string]$ReferenceInstallRoot,
	[Parameter(Mandatory)][string]$ReferenceReceipt,
	[Parameter(Mandatory)][string]$WorkRoot,
	# Supply the existing bundled Python runtime; global Python is not selected.
	[Parameter(Mandatory)][string]$PythonPath,
	[Parameter(Mandatory)][string]$RepositoryRoot,
	[ValidateSet('small','normal','stress')][string[]]$Scales=@('small','normal','stress'),
	# The normal Build bundle is beside its matching import library. A relocated
	# bundle needs an explicit matching library, never a candidate API/old DLL mix.
	[string]$CandidateImportLibrary='',
	[Parameter(Mandatory)][string]$ReferenceClosureReceipt,
	[Parameter(Mandatory)][string]$FrozenToolRoot,
	[Parameter(Mandatory)][string]$RendererSourceProofPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$utf8=[Text.UTF8Encoding]::new($false)
function Require([bool]$Condition,[string]$Message){if(!$Condition){throw $Message}}
function Full([string]$Path){[IO.Path]::GetFullPath($Path)}
function Below([string]$Child,[string]$Root){$Child.StartsWith($Root.TrimEnd('/','\')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)}
function NoRedirect([string]$Path){
	$cursor=Full $Path
	while($cursor){
		if(Test-Path -LiteralPath $cursor){Require (!((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) ('Redirected path: '+$cursor)}
		$cursor=[IO.Path]::GetDirectoryName($cursor)
	}
}
function Pin([string]$Path){
	NoRedirect $Path
	$before=Get-Item -LiteralPath $Path
	Require (!$before.PSIsContainer) ('Expected regular file: '+$Path)
	$bytes=$before.Length;$time=$before.LastWriteTimeUtc.Ticks
	$hash=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
	$after=Get-Item -LiteralPath $Path
	Require ($after.Length -eq $bytes -and $after.LastWriteTimeUtc.Ticks -eq $time) ('Input changed during hashing: '+$Path)
	return [pscustomobject]@{path=(Full $Path);bytes=$bytes;sha256=$hash}
}
function VerifyPin($Value){
	$now=Pin $Value.path
	Require ($now.bytes -eq $Value.bytes -and $now.sha256 -ceq $Value.sha256) ('Pinned input changed: '+$Value.path)
}
function JsonWrite([string]$Path,$Value){
	Require (!(Test-Path -LiteralPath $Path)) ('Output must be fresh: '+$Path)
	[IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 90),$utf8)
}
function JsonRead([string]$Path){Get-Content -LiteralPath $Path -Raw|ConvertFrom-Json}
function CopyPinnedFile([string]$Source,[string]$Destination){
	$sourcePin=Pin $Source
	Require (!(Test-Path -LiteralPath $Destination)) ('Staging destination exists: '+$Destination)
	[IO.Directory]::CreateDirectory((Split-Path $Destination))|Out-Null
	Copy-Item -LiteralPath $Source -Destination $Destination
	$stagedPin=Pin $Destination
	Require ($sourcePin.sha256 -ceq $stagedPin.sha256 -and $sourcePin.bytes -eq $stagedPin.bytes) 'Staged bytes differ.'
	return [pscustomobject]@{source=$sourcePin;staged=$stagedPin}
}
$aggregate=[ordered]@{schemaVersion='runtime-memory-safety-parity-run-v1';status='Unverified';
	startedUtc=[DateTime]::UtcNow.ToString('o');nativeAcceptance=$false;performanceAcceptance=$false;
	privateReferenceCapturedCameraArraysObserved=$false;candidateCapturedArraysRequireSeparateNativeRegression=$true;
	timedPostImportAssetClosureVerified=$false;commands=@();hosts=@();comparisons=@()}
$commands=[Collections.Generic.List[object]]::new()
$hosts=[Collections.Generic.List[object]]::new()
$comparisons=[Collections.Generic.List[object]]::new()
$ownedWork=$false;$work='';$exitCode=2
function InvokeLogged([string]$Name,[string]$Executable,[string[]]$Arguments){
	$log=Join-Path $work ('command-'+$commands.Count.ToString('d2')+'-'+$Name+'.log')
	$record=[ordered]@{name=$Name;executable=(Pin $Executable);arguments=$Arguments;startedUtc=[DateTime]::UtcNow.ToString('o');exitCode=$null;log=$null}
	$commands.Add($record)
	# Child PowerShell isolates scripts that use exit or inherit LASTEXITCODE.
	# Argument arrays avoid shell text composition and preserve paths with spaces.
	& $Executable @Arguments *> $log
	$code=$LASTEXITCODE
	$record.exitCode=$code;$record.completedUtc=[DateTime]::UtcNow.ToString('o');$record.log=Pin $log
	return [int]$code
}
function InvokeCmakeLogged([string]$Name,[string[]]$Arguments){
	$log=Join-Path $work ('command-'+$commands.Count.ToString('d2')+'-'+$Name+'.log')
	$record=[ordered]@{name=$Name;executable=(Pin $buildHelper);arguments=$Arguments;startedUtc=[DateTime]::UtcNow.ToString('o');exitCode=$null;log=$null}
	$commands.Add($record)
	# Keep the helper's CMake string[] typed. PowerShell -File does not preserve
	# an array parameter reliably; the existing helper restores its environment.
	& $buildHelper -CMakeArguments $Arguments *> $log
	$code=$LASTEXITCODE;$record.exitCode=$code;$record.completedUtc=[DateTime]::UtcNow.ToString('o');$record.log=Pin $log
	return [int]$code
}
try{
	foreach($path in @($CandidateInstallRoot,$ReferenceInstallRoot,$ReferenceReceipt,$WorkRoot,$PythonPath,$RepositoryRoot,$ReferenceClosureReceipt,$FrozenToolRoot)){
		Require ([IO.Path]::IsPathRooted($path)) 'Use absolute paths for every input/workroot.';NoRedirect $path
	}
	$candidate=Full $CandidateInstallRoot;$reference=Full $ReferenceInstallRoot;$archiveReceipt=Full $ReferenceReceipt;$work=Full $WorkRoot
	$python=Full $PythonPath;$draft=Full $PSScriptRoot;$repo=Full $RepositoryRoot
	Require ([IO.Path]::IsPathRooted($FrozenToolRoot)) 'FrozenToolRoot must be absolute.';$frozenTools=Full $FrozenToolRoot;NoRedirect $frozenTools
	$referenceArchive=Split-Path $reference
	Require ([IO.Path]::IsPathRooted($RendererSourceProofPath)) 'RendererSourceProofPath must be absolute.'
	$rendererProof=Full $RendererSourceProofPath;$rendererSource=JsonRead $rendererProof
	VerifyPin $rendererSource.referenceSource;VerifyPin $rendererSource.candidateSource;VerifyPin $rendererSource.referenceCompatibilityCapacityHeader
	foreach($source in $rendererSource.additionalSources){VerifyPin $source.referenceSource;VerifyPin $source.candidateSource}
	Require ($rendererSource.sourceRevision -ceq (JsonRead $archiveReceipt).sourceRevision -and $rendererSource.referenceGitBlob -cmatch '^[0-9a-f]{40}$') 'Renderer source proof revision differs from reference receipt.'
	Require ((Full $rendererSource.candidateSource.path) -ieq (Join-Path $repo 'Engine/Renderer/RenderSystem.cpp')) 'Renderer candidate source proof is not current normal renderer.'

	Require ((Test-Path -LiteralPath $candidate -PathType Container) -and (Test-Path -LiteralPath $reference -PathType Container)) 'Missing installed runtime root.'
	Require (!(Test-Path -LiteralPath $work)) 'WorkRoot must be fresh and nonexistent.'
	Require ($Scales.Count -gt 0 -and @($Scales|Select-Object -Unique).Count -eq $Scales.Count) 'Scales must be nonempty and unique.'
	$protectedSources=@($candidate,$referenceArchive,$draft,$frozenTools,$referenceArchive)+@('Engine','Editor','Tests','External','specs','graphify-out','.agents','.codex','.specify'|ForEach-Object {Join-Path $repo $_})
	foreach($source in $protectedSources){
		Require ($work -ine $source -and !(Below $work $source) -and !(Below $source $work)) 'WorkRoot overlaps runtime/source data.'
	}
	Require ($candidate -ine $reference -and !(Below $candidate $reference) -and !(Below $reference $candidate)) 'Candidate/reference runtime roots overlap.'
	$referenceHeaders=Join-Path $referenceArchive 'Compatibility/Engine'
	$referenceLibrary=Join-Path $referenceArchive 'Compatibility/Libraries/TerraEngine.lib'
	if(!$CandidateImportLibrary){$CandidateImportLibrary=Join-Path (Split-Path $candidate) 'TerraEngine.lib'}
	Require ([IO.Path]::IsPathRooted($CandidateImportLibrary)) 'CandidateImportLibrary must be absolute.'
	$candidateLibrary=Full $CandidateImportLibrary
	Require ($candidateLibrary -ine $referenceLibrary -and !(Below $candidateLibrary $referenceArchive)) 'Candidate import library cannot come from the reference archive.'
	$buildHelper=Join-Path $repo 'Tests/Tools/Locomotion/InvokeLocomotionBuild.ps1'
	# The frozen PS adapter's selfhash is the generator identity. Native bootstrap
	# selection still comes from each runtime's -InstallRoot and is distinct.
	$materializer=Join-Path $frozenTools 'Tests/Tools/RuntimeMemorySafety/MaterializeRuntimeMemorySafetyFixture.ps1'
	$manifest=Join-Path $frozenTools 'Tests/Tools/RuntimeMemorySafety/Fixtures/manifest.json'
	$watchdog=Join-Path $frozenTools 'Tests/Tools/Correctness/RunWithWatchdog.ps1'
	$comparator=Join-Path $draft 'Compare-RuntimeMemorySafetyParity.py'
	$qualifier=Join-Path $draft 'Qualify-RuntimeMemorySafetyParityProject.ps1'
	$pwsh=Join-Path $PSHOME 'pwsh.exe'
	$sourcePaths=@($PSCommandPath,(Join-Path $draft 'CMakeLists.txt'),(Join-Path $draft 'RuntimeMemorySafetyParityHost.cpp'),
		$comparator,$qualifier,$ReferenceClosureReceipt,$rendererProof,$rendererSource.referenceSource.path,$rendererSource.candidateSource.path,$rendererSource.referenceCompatibilityCapacityHeader.path,(Join-Path $draft 'support-pins.json'),$buildHelper,$materializer,$manifest,$watchdog,
		$referenceLibrary,$candidateLibrary,$archiveReceipt,$python,$pwsh,
		(Join-Path $repo 'Engine/Renderer/RHI/Vulkan/VulkanReadbackConversion.cpp'))
	foreach($runtime in @($reference,$candidate)){
		Require (Test-Path -LiteralPath (Join-Path $runtime 'Editor') -PathType Container) 'Installed Editor runtime closure missing.'
		$sourcePaths+=@(Get-ChildItem -LiteralPath (Join-Path $runtime 'Editor') -File -Filter '*.dll'|ForEach-Object FullName)
		foreach($relative in @('Editor/TerraEngine.dll','Editor/Managed/TerraEngine.CSharp.dll','Editor/Managed/TerraEngine.CSharp.runtimeconfig.json','Editor/Managed/hostfxr.dll')){
			$sourcePaths+=Join-Path $runtime $relative
		}
	}
	$headerRoots=@($referenceHeaders,(Join-Path $repo 'Engine'),(Join-Path $draft 'support'),
		(Join-Path $repo 'External/entt/src'),(Join-Path $repo 'External/glm'),(Join-Path $repo 'External/json/single_include'),
		(Join-Path $repo 'External/volk'),(Join-Path $repo 'External/VMA/include'),
		(Join-Path $repo 'External/Vulkan-Headers/include'),(Join-Path $repo 'External/glfw/include'))
	foreach($root in $headerRoots){
		NoRedirect $root;Require (Test-Path -LiteralPath $root -PathType Container) ('Header root missing: '+$root)
		$sourcePaths+=@(Get-ChildItem -LiteralPath $root -Recurse -File|Where-Object {$_.Extension -in @('.h','.hpp','.inl')}|ForEach-Object FullName)
	}
	$sourcePaths+=@($rendererSource.additionalSources|ForEach-Object {$_.referenceSource.path;$_.candidateSource.path})
	$sourcePins=@($sourcePaths|Sort-Object -Unique|ForEach-Object {Pin $_})
	$support=JsonRead (Join-Path $draft 'support-pins.json')
	# Support declaration identities are portable source pins. Runtime/archive
	# evidence is independently supplied and pinned for this run.
	foreach($row in $support.support){
		Require (![IO.Path]::IsPathRooted($row.path)) 'Support pin must be relative.'
		$resolved=Full (Join-Path $draft $row.path);Require (Below $resolved $draft) 'Support pin escaped package.'
		VerifyPin ([pscustomobject]@{path=$resolved;bytes=$row.bytes;sha256=$row.sha256})
	}
	[IO.Directory]::CreateDirectory($work)|Out-Null;$ownedWork=$true
	JsonWrite (Join-Path $work 'source-inputs.json') $sourcePins
	$aggregate.sourceInputs=Pin (Join-Path $work 'source-inputs.json')
	$aggregate.referenceReceipt=Pin $archiveReceipt;$aggregate.requestedScales=$Scales
	$aggregate.referencePostImportClosureReceipt=Pin $ReferenceClosureReceipt
	$aggregate.rendererSourceProof=Pin $rendererProof
	$aggregate.frozenToolRoot=$frozenTools
	$environment=[ordered]@{os=[Environment]::OSVersion.VersionString;processArchitecture=[Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString();values=[ordered]@{}}
	foreach($name in @('TERRAENGINE_THREADPOOL_WORKERS','VK_ICD_FILENAMES','VK_DRIVER_FILES','VK_LAYER_PATH','DOTNET_ROOT','DOTNET_ROOT_X64','DOTNET_MULTILEVEL_LOOKUP')){
		$environment.values[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
	}
	JsonWrite (Join-Path $work 'inherited-environment.json') $environment;$aggregate.environment=Pin (Join-Path $work 'inherited-environment.json')
	$build=Join-Path $work 'host-build'
	$configure=@('-S',$draft,'-B',$build,'-G','Ninja',
		'-DCMAKE_BUILD_TYPE=RelWithDebInfo',('-DTERRA_REPOSITORY='+$repo),('-DTERRA_REFERENCE='+$referenceArchive),('-DTERRA_CANDIDATE_LIBRARY='+$candidateLibrary))
	Require ((InvokeCmakeLogged 'configure' $configure) -eq 0) 'Parity narrow configure failed.'
	Require ((InvokeCmakeLogged 'build' @(
		'--build',$build,'--target','RuntimeMemorySafetyParityBaseline','RuntimeMemorySafetyParityCandidate','--parallel','2')) -eq 0) 'Parity two-target build failed.'
	foreach($sourcePin in $sourcePins){VerifyPin $sourcePin}
	foreach($variant in @('Baseline','Candidate')){
		$runtime=if($variant -ceq 'Baseline'){$reference}else{$candidate};$stage=Join-Path $work $variant
		[IO.Directory]::CreateDirectory($stage)|Out-Null
		$staged=[Collections.Generic.List[object]]::new()
		$staged.Add((CopyPinnedFile (Join-Path $build "$variant/RuntimeMemorySafetyParity$variant.exe") (Join-Path $stage "RuntimeMemorySafetyParity$variant.exe")))
		foreach($file in Get-ChildItem -LiteralPath (Join-Path $runtime 'Editor') -File -Filter '*.dll'){$staged.Add((CopyPinnedFile $file.FullName (Join-Path $stage $file.Name)))}
		foreach($file in Get-ChildItem -LiteralPath (Join-Path $runtime 'Editor/Managed') -Recurse -File){
			$relative=[IO.Path]::GetRelativePath((Join-Path $runtime 'Editor/Managed'),$file.FullName)
			$staged.Add((CopyPinnedFile $file.FullName (Join-Path (Join-Path $stage 'Managed') $relative)))
		}
		JsonWrite (Join-Path $work "$variant-staging.json") ($staged.ToArray())
		$aggregate[($variant.ToLowerInvariant()+'Staging')]=Pin (Join-Path $work "$variant-staging.json")
	}
	foreach($scale in $Scales){
		foreach($variant in @('Baseline','Candidate')){
			$runtime=if($variant -ceq 'Baseline'){$reference}else{$candidate};$stage=Join-Path $work $variant
			$fixture=Join-Path $work "$scale-$variant-fixture";$capture=Join-Path $work "$scale-$variant-capture";$process=Join-Path $work "$scale-$variant-process"
			$lane=[ordered]@{scale=$scale;variant=$variant;status='Unverified';materialization=$null;hostReceipt=$null;watchdogReceipt=$null}
			$hosts.Add($lane)
			Require ((InvokeLogged "$scale-$variant-materialize" $pwsh @('-NoProfile','-ExecutionPolicy','Bypass','-File',$materializer,
				'-InstallRoot',$runtime,'-WorkRoot',$fixture,'-Scale',$scale)) -eq 0) 'Fresh materialization failed.'
			$lane.materialization=Pin (Join-Path $fixture 'materialization.json')
			$qualification=Join-Path $fixture 'qualification.json'
			Require ((InvokeLogged "$scale-$variant-qualify" $pwsh @('-NoProfile','-ExecutionPolicy','Bypass','-File',$qualifier,
				'-ReferenceClosureReceipt',$ReferenceClosureReceipt,'-ProjectRoot',(Join-Path $fixture 'project'),'-Materialization',$lane.materialization.path,
				'-Scale',$scale,'-OutputPath',$qualification)) -eq 0) 'Full actual post-import runtime qualification failed.'
			$lane.qualification=Pin $qualification
			# RunWithWatchdog's native launch needs an array argument. Invoke its
			# exact typed parameters inside this process instead of flattening that
			# array through powershell.exe -File, whose CLI binder cannot retain it.
			$log=Join-Path $work "$scale-$variant-watchdog.log"
			$hostArguments=@('--runtime-root',$runtime,'--project-root',(Join-Path $fixture 'project'),'--materialization',
				(Join-Path $fixture 'materialization.json'),'--qualification',$qualification,'--renderer-source-proof',$rendererProof,'--support-manifest',(Join-Path $draft 'support-pins.json'),'--output-root',$capture,'--archive-receipt',$archiveReceipt)
			$command=[ordered]@{name="$scale-$variant-watchdog";executable=(Pin $watchdog);arguments=$hostArguments;workingDirectory=$stage;startedUtc=[DateTime]::UtcNow.ToString('o');exitCode=$null;log=$null}
			$commands.Add($command)
			& $watchdog -Executable (Join-Path $stage "RuntimeMemorySafetyParity$variant.exe") -WorkingDirectory $stage -TimeoutSeconds 180 -LogDirectory $process -ArgumentList $hostArguments *> $log
			$code=$LASTEXITCODE;$command.exitCode=$code;$command.completedUtc=[DateTime]::UtcNow.ToString('o');$command.log=Pin $log
			if(Test-Path -LiteralPath (Join-Path $process 'watchdog.json')){$lane.watchdogReceipt=Pin (Join-Path $process 'watchdog.json')}
			if(Test-Path -LiteralPath (Join-Path $capture 'parity-receipt.json')){$lane.hostReceipt=Pin (Join-Path $capture 'parity-receipt.json')}
			if($code -ne 0){$aggregate.status=if($code -eq 1){'Failed'}else{'Unverified'};$exitCode=if($code -eq 1){1}else{2}}
			Require ($code -eq 0) ('Host failed or unverified: '+$scale+' '+$variant+' exit '+$code)
			$watch=JsonRead $lane.watchdogReceipt.path;$native=JsonRead $lane.hostReceipt.path
			Require ($watch.exitCode -eq 0 -and !$watch.timedOut -and $native.status -ceq 'Captured' -and $native.normalShutdownCompleted) 'Host lacks successful normal completion JSON.'
			$lane.status='Captured'
		}
		$comparison=Join-Path $work "$scale-comparison"
		$comparisonLane=[ordered]@{scale=$scale;status='Unverified';receipt=$null};$comparisons.Add($comparisonLane)
		$code=InvokeLogged "$scale-compare" $python @('-B',$comparator,'--scale',$scale,'--baseline',(Join-Path $work "$scale-Baseline-capture/parity-receipt.json"),
			'--candidate',(Join-Path $work "$scale-Candidate-capture/parity-receipt.json"),'--fixture-manifest',$manifest,'--output',$comparison)
		if(Test-Path -LiteralPath (Join-Path $comparison 'comparison.json')){$comparisonLane.receipt=Pin (Join-Path $comparison 'comparison.json')}
		if($code -ne 0){
			$aggregate.status=if($code -eq 1){'Different'}else{'Unverified'};$exitCode=if($code -eq 1){1}else{2}
			if($null -ne $comparisonLane.receipt){$comparisonLane.status=(JsonRead $comparisonLane.receipt.path).status}
		}
		Require ($code -eq 0) ('Strict comparison differs or is unverified: '+$scale+' exit '+$code)
		$comparisonJson=JsonRead $comparisonLane.receipt.path
		Require ($comparisonJson.status -ceq 'ComparedEqual' -and !$comparisonJson.nativeAcceptance -and !$comparisonJson.performanceAcceptance -and
			$comparisonJson.candidateCapturedArraysObservedAndEqual) 'Comparator result missing actual candidate capture proof or claims unsupported acceptance.'
		$comparisonLane.actualCandidateFallbackAllocationCount=$comparisonJson.actualCandidateFallbackAllocationCount
		$comparisonLane.candidateScratch=$comparisonJson.candidateScratch
		$comparisonLane.status='ComparedEqual'
	}
	foreach($sourcePin in $sourcePins){VerifyPin $sourcePin}
	Require ($comparisons.Count -eq $Scales.Count) 'Requested comparison set incomplete.'
	$aggregate.status='CompletedComparisons';$aggregate.allRequestedScalesComparedEqual=$true;$exitCode=0
}catch{
	$aggregate.reason=$_.Exception.Message
	Write-Error $aggregate.reason -ErrorAction Continue
}finally{
	if($ownedWork){
		$aggregate.commands=$commands.ToArray();$aggregate.hosts=$hosts.ToArray();$aggregate.comparisons=$comparisons.ToArray();$aggregate.completedUtc=[DateTime]::UtcNow.ToString('o')
		JsonWrite (Join-Path $work 'parity-run.json') $aggregate
		Write-Output ('Parity aggregate: '+(Join-Path $work 'parity-run.json'))
	}
}
exit $exitCode
