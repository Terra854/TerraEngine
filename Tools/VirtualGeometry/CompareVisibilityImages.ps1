[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Baseline,[Parameter(Mandatory=$true)][string]$Candidate,[Parameter(Mandatory=$true)][string]$Output,[Parameter(Mandatory=$true)][ValidateSet('hardware','compute','indexed','hybrid')][string]$BaselineRoute,[Parameter(Mandatory=$true)][ValidateSet('hardware','compute','indexed','hybrid')][string]$CandidateRoute,[switch]$DryRun)
$scriptPath=Join-Path $PSScriptRoot 'CompareVirtualGeometryImages.ps1'
if(-not(Test-Path $scriptPath)){throw "Canonical image comparator is missing: $scriptPath"}
$args=@('-NoProfile','-File',$scriptPath,'-Baseline',$Baseline,'-Candidate',$Candidate,'-Output',$Output)
if($DryRun){$args += '-DryRun'}
& powershell @args
if($LASTEXITCODE -eq 0 -and -not $DryRun){
	$comparison=Get-Content -LiteralPath $Output -Raw|ConvertFrom-Json
	# Both inputs are independently route-locked at capture time. Recording the
	# pair preserves that provenance while allowing the intended cross-route
	# parity comparison (hardware versus compute, indexed, or hybrid).
	$comparison|Add-Member -NotePropertyName baselineVisibilityRoute -NotePropertyValue $BaselineRoute
	$comparison|Add-Member -NotePropertyName candidateVisibilityRoute -NotePropertyValue $CandidateRoute
	$comparison|ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding utf8
}
exit $LASTEXITCODE
