<#
Parent-approved validation-only qualification: copy the complete observed actual
post-import runtime closure into one fresh materialized derivative. Original
materialization, reference/calibration and frozen tools remain untouched.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$ReferenceClosureReceipt,
	[Parameter(Mandatory)][string]$ProjectRoot,
	[Parameter(Mandatory)][string]$Materialization,
	[Parameter(Mandatory)][ValidateSet('small','normal','stress')][string]$Scale,
	[Parameter(Mandatory)][string]$OutputPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Require([bool]$Condition,[string]$Message){if(!$Condition){throw $Message}}
function Full([string]$Path){[IO.Path]::GetFullPath($Path)}
function Below([string]$Child,[string]$Root){$Child.StartsWith($Root.TrimEnd('/','\')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)}
function NoRedirect([string]$Path){$p=Full $Path;while($p){if(Test-Path -LiteralPath $p){Require (!((Get-Item -LiteralPath $p -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Redirected qualification path.'};$p=[IO.Path]::GetDirectoryName($p)}}
function Pin([string]$Path){NoRedirect $Path;$f=Get-Item -LiteralPath $Path;Require (!$f.PSIsContainer) 'Expected regular qualification file.';return [pscustomobject]@{path=(Full $Path);bytes=$f.Length;sha256=(Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant()}}
function Verify($Value){$p=Pin $Value.path;Require ($p.bytes -eq $Value.bytes -and $p.sha256 -ceq $Value.sha256) ('Qualified source changed: '+$Value.path)}
function ReadJson([string]$Path){Get-Content -LiteralPath $Path -Raw|ConvertFrom-Json}
function JsonWrite([string]$Path,$Value){Require (!(Test-Path -LiteralPath $Path)) 'Qualification output must be fresh.';[IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 60),[Text.UTF8Encoding]::new($false))}
$paths=@('Build/Assets/.terra-animation-manifests/da35202f311bd5ea56e23beb0aafbd9a.json',
	'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_493d9bc3.teanim',
	'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_fad19fd5.teanim',
	'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_9beae902.teskel',
	'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_b7d2176c.teskel',
	'Build/Assets/Models/ImportedHero_Generated/ImportedHeroBody.temf','Build/Assets/Models/ImportedHero_Generated/ImportedHeroMaterial.tmat',
	'ProjectSettings/RenderSettings.json','Assets/Scenes/Scene.tescene')
foreach($p in @($ReferenceClosureReceipt,$ProjectRoot,$Materialization,$OutputPath)){Require ([IO.Path]::IsPathRooted($p)) 'Qualification requires absolute paths.';NoRedirect $p}
$project=Full $ProjectRoot;$output=Full $OutputPath;$materializationPath=Full $Materialization
Require (!(Test-Path -LiteralPath $output) -and (Split-Path $output) -ieq (Split-Path $materializationPath) -and $output -ine $materializationPath) 'Qualification receipt must be fresh beside original materialization.'
$initial=ReadJson $materializationPath
Require ($initial.kind -ceq 'RuntimeMemorySafetyValidationProject' -and $initial.scale -ceq $Scale -and (Full $initial.projectRoot) -ieq $project) 'Fresh runtime-memory-safety project identity differs.'
foreach($row in $initial.inputClosure){$p=Pin (Join-Path $project $row.projectPath);Require ($p.sha256 -ceq $row.sha256 -and $p.bytes -eq $row.sizeBytes) 'Project is not fresh original input closure.'}
foreach($property in $initial.generatedHashes.PSObject.Properties){Require ((Pin (Join-Path $project $property.Name)).sha256 -ceq $property.Value) 'Fresh generated input differs.'}
$referencePin=Pin $ReferenceClosureReceipt;$reference=ReadJson $ReferenceClosureReceipt
Require ($reference.status -ceq 'ReferenceObserved' -and $reference.referenceCookedAndSettingsIdenticalAcross18 -and $reference.referenceSceneIdenticalWithinEachScale -and $reference.references.Count -eq 18) 'Require sealed valid18-lane supplemental reference observation.'
$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach($lane in $reference.references){
	Require ($seen.Add($lane.lane)) 'Duplicate reference closure lane.'
	Verify $lane.capture;Verify $lane.lifecycle;Verify $lane.materialization
	$properties=@($lane.runtime.PSObject.Properties.Name|Sort-Object)
	Require (($properties -join "`n") -ceq (($paths|Sort-Object)-join "`n")) 'Reference runtime path set differs.'
	foreach($relative in $paths){Verify $lane.runtime.$relative;Require (!(Below $project (Split-Path $lane.runtime.$relative.path)) -and $project -ine (Split-Path $lane.runtime.$relative.path)) 'Derivative overlaps observed source.'}
}
foreach($s in @('small','normal','stress')){foreach($role in @('A','Aprime')){foreach($n in 1..3){Require ($seen.Contains("$s-$role$n")) 'Missing reference closure lane.'}}}
$selected=@($reference.references|Where-Object lane -ceq "$Scale-A1")[0]
$sourceMaterialization=ReadJson $selected.materialization.path
foreach($relative in @('Assets/Scenes/Scene.tescene','runtime-memory-safety-cameras.json','runtime-memory-safety-lights.json')){
	Require ($sourceMaterialization.generatedHashes.$relative -ceq $initial.generatedHashes.$relative) 'Frozen scene/camera/light identity changed.'
}
foreach($lane in $reference.references){foreach($relative in $paths){if($relative -cne 'Assets/Scenes/Scene.tescene' -or $lane.scale -ceq $Scale){Require ($lane.runtime.$relative.sha256 -ceq $selected.runtime.$relative.sha256) 'Actual reference closure equivalence changed.'}}}
$manifestRelative=$paths[0];$originalManifestPath=Join-Path (Split-Path $output) 'qualification-original-package-manifest.json'
Require (!(Test-Path -LiteralPath $originalManifestPath)) 'Original package snapshot must be fresh.'
Copy-Item -LiteralPath (Join-Path $project $manifestRelative) -Destination $originalManifestPath
$oldManifest=ReadJson $originalManifestPath;$newManifest=ReadJson $selected.runtime.$manifestRelative.path
$oldMaterial=@($oldManifest.subAssets|Where-Object kind -ceq 'Material');$newMaterial=@($newManifest.subAssets|Where-Object kind -ceq 'Material')
Require ($oldMaterial.Count -eq 1 -and $newMaterial.Count -eq 1 -and $oldMaterial[0].guid -ceq $newMaterial[0].guid) 'Material UUID semantics conflict.'
$meshSource=$selected.runtime.'Build/Assets/Models/ImportedHero_Generated/ImportedHeroBody.temf'
$meshBytes=[IO.File]::ReadAllBytes($meshSource.path)
Require ($meshBytes.Length -ge 192 -and [BitConverter]::ToUInt32($meshBytes,8) -eq 192 -and [Text.Encoding]::ASCII.GetString($meshBytes,168,8) -ceq 'TEMFPRD1') 'Observed mesh lacks canonical TEMFPRD1 semantic/header identity.'
$files=@(foreach($relative in $paths){
	$target=Full (Join-Path $project $relative);Require (Below $target $project) 'Qualification target escaped derivative.';NoRedirect $target
	$before=if(Test-Path -LiteralPath $target){Pin $target}else{$null}
	$source=$selected.runtime.$relative;Verify $source
	[IO.Directory]::CreateDirectory((Split-Path $target))|Out-Null
	Copy-Item -LiteralPath $source.path -Destination $target -Force
	$qualified=Pin $target;Require ($qualified.sha256 -ceq $source.sha256 -and $qualified.bytes -eq $source.bytes) 'Qualified copy differs.'
	[pscustomobject]@{relative=$relative;original=$before;source=$source;qualified=$qualified}
})
$result=[ordered]@{schemaVersion='runtime-memory-safety-qualified-runtime-v1';status='Qualified';scale=$Scale;
	policy='FullActualPostImportReferenceRuntimeClosure';projectRoot=$project;originalMaterialization=(Pin $materializationPath);
	referenceClosureReceipt=$referencePin;referenceCapture=$selected.capture;referenceLifecycle=$selected.lifecycle;
	originalPackageManifest=(Pin $originalManifestPath);files=$files;
	canonicalMeshHeader=@{headerBytes=192;formatContractOffset=168;formatContractId='TEMFPRD1';source=$meshSource};
	nativeAcceptance=$false;performanceAcceptance=$false}
JsonWrite $output $result
Write-Output $output
exit 0
