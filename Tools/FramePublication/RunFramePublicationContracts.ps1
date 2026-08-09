[CmdletBinding()]
param([string]$InstallRoot,[string]$Manifest,[string]$OutputDirectory,[string]$Executable)
$ErrorActionPreference='Stop'; if(-not(Test-Path $Manifest)){throw "Manifest is missing: $Manifest"}; $m=Get-Content -Raw $Manifest|ConvertFrom-Json
if($m.scenarios.Count -ne 6){throw 'Canonical scenario set is incomplete.'}; New-Item -ItemType Directory -Force $OutputDirectory|Out-Null
if([string]::IsNullOrWhiteSpace($Executable)){
	$candidate=Join-Path $InstallRoot 'Tools/FramePublication/TerraEngine-FramePublicationContracts.exe'; if(Test-Path $candidate){$Executable=$candidate}else{throw 'Native contract executable is not configured; no fabricated evidence was written.'}
}
if(-not(Test-Path $Executable)){throw "Contract executable is missing: $Executable"}
& $Executable '--frame-publication-contracts' '--manifest' (Resolve-Path $Manifest) '--output' (Resolve-Path $OutputDirectory)
if($LASTEXITCODE -ne 0){throw "Contract executable failed with exit code $LASTEXITCODE."}
