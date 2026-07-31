[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Baseline,[Parameter(Mandatory=$true)][string]$Candidate,[Parameter(Mandatory=$true)][string]$Output,[int]$MaxDifferentPixels=0,[switch]$DryRun)
Set-StrictMode -Version Latest; $ErrorActionPreference='Stop'
foreach($p in @($Baseline,$Candidate)){if(-not(Test-Path -LiteralPath $p -PathType Leaf)){throw "Image artifact missing: $p"}}
if($DryRun){Write-Output 'validated image artifacts (dry run; no pixel claim)';exit 0}
$a=[IO.File]::ReadAllBytes((Resolve-Path $Baseline));$b=[IO.File]::ReadAllBytes((Resolve-Path $Candidate));
$different=0;for($i=0;$i -lt [Math]::Max($a.Length,$b.Length);$i++){if(($i -ge $a.Length)-or($i -ge $b.Length)-or$a[$i]-ne$b[$i]){$different++}}
$result=[ordered]@{schemaVersion=1;status=if($different -le $MaxDifferentPixels){'accepted'}else{'mismatch'};baselineSha256=(Get-FileHash $Baseline -Algorithm SHA256).Hash.ToLowerInvariant();candidateSha256=(Get-FileHash $Candidate -Algorithm SHA256).Hash.ToLowerInvariant();differentBytes=$different;maxDifferentBytes=$MaxDifferentPixels;note='Byte comparison is conservative; unavailable images fail before comparison.'}
$result|ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding utf8;if($result.status -ne 'accepted'){exit 2}
