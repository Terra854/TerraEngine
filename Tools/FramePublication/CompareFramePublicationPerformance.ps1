[CmdletBinding()]
param([string]$BaselineDirectory,[string]$CandidateDirectory,[string]$Output)
$ErrorActionPreference='Stop'
function Load([string]$dir){ $file=Join-Path $dir 'capture.json'; if(-not (Test-Path $file)){ throw "Capture manifest missing: $file" }; Get-Content -Raw $file | ConvertFrom-Json }
$base=Load $BaselineDirectory; $candidate=Load $CandidateDirectory
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null
if([string]$base.status -eq 'incomparable' -or $null -eq $base.identity){
	$result=[ordered]@{comparable=$false;identity=$null;distributions=@{};reason='Historical baseline is explicitly incomparable; no identity or delta was backfilled.'}
	$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output
	exit 2
}
if(@($base.runs).Count -ne 5 -or @($candidate.runs).Count -ne 5){
	$result=[ordered]@{comparable=$false;identity=$candidate.identity;distributions=@{};reason='Comparable captures require exactly five measured runs.'}
	$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output
	exit 2
}
$fields=@(
	'scenario','scenarioVersion','projectHash','sceneHash','cameraHash','population',
	'animationPolicy','routeMix','specialistMask','seed','settingsHash','powerState',
	'buildHash','dirtyDiffHash','installedHash','workerCount','profilerHash','adapter',
	'driver','operatingSystem','buildConfiguration','backend','timingSources',
	'workloadManifestHash','liveAdapterHash','captureSchemaVersion','profilerDetailLevel',
	'measuredDurationSeconds','viewPolicy','contentHashes')
$mismatch=@()
foreach($field in $fields){
	$baseProperty=$base.identity.PSObject.Properties[$field]
	$candidateProperty=$candidate.identity.PSObject.Properties[$field]
	if($null -eq $baseProperty -or $null -eq $candidateProperty){
		$mismatch+="$field (missing)"
		continue
	}
	$a=($baseProperty.Value | ConvertTo-Json -Compress -Depth 20)
	$b=($candidateProperty.Value | ConvertTo-Json -Compress -Depth 20)
	if($a -ne $b){$mismatch+=$field}
}
foreach($capture in @($base,$candidate)){
	if($null -eq $capture.warmup -or [int]$capture.warmup.seconds -lt 30 -or
		[UInt64]$capture.warmup.completedFrames -lt 120){
		$mismatch+='warmup (missing or incomplete)'
	}
	foreach($run in @($capture.runs)){
		foreach($source in @($capture.identity.timingSources)){
			if($null -eq $run.timings -or $null -eq $run.timings.PSObject.Properties[[string]$source]){
				$mismatch+="timing source '$source' (missing)"
			}
		}
	}
}
$mismatch=@($mismatch | Select-Object -Unique)
$result=[ordered]@{comparable=($mismatch.Count -eq 0);identity=$candidate.identity;distributions=@{};reason=if($mismatch.Count){'Identity mismatch: '+($mismatch -join ', ')}else{$null}}
function Stats($values){
	$valid=@($values | Where-Object {$_ -ne $null} | ForEach-Object {[double]$_} | Sort-Object)
	if($valid.Count -eq 0){return [ordered]@{median=$null;p95=$null;p99=$null;worst=$null;missing=$values.Count}}
	$pick={param([double]$p) $valid[[Math]::Min($valid.Count-1,[Math]::Max(0,[int][Math]::Ceiling($p*$valid.Count)-1))]}
	return [ordered]@{median=&$pick .5;p95=&$pick .95;p99=&$pick .99;worst=$valid[-1];missing=$values.Count-$valid.Count}
}
if($result.comparable){ foreach($name in @('engineCpu','rendererCpu','animationCpu','gpuTimestamp')){
		$baseValues=@($base.runs | ForEach-Object {$_.timings.$name}); $candidateValues=@($candidate.runs | ForEach-Object {$_.timings.$name});
		$cs=Stats $candidateValues; $bs=Stats $baseValues; $delta=$null; if($cs.median -ne $null -and $bs.median -ne $null -and $bs.median -ne 0){$delta=($cs.median-$bs.median)/$bs.median*100.0}; $cs.pairedDelta=$delta; $result.distributions[$name]=$cs
	} }
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output
if(-not $result.comparable){ exit 2 }
