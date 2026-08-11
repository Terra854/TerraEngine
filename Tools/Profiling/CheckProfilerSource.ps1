[CmdletBinding()]
param(
	[string]$RepositoryRoot = (Join-Path $PSScriptRoot '../..'),
	[string]$InstallRoot = '',
	[switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# This audit is deliberately textual and fail-closed.  A format migration is
# not complete when an old reader is merely unreachable; old symbols in source
# or the install tree can still make the accepted format ambiguous.
$root = [IO.Path]::GetFullPath($RepositoryRoot)
if(-not (Test-Path -LiteralPath $root -PathType Container)) { throw "Repository root does not exist: $root" }
$scanRoots = @('Engine/Profiling','Editor','Tools/Profiling','Tests/Engine','Tests/Editor','Docs') |
	ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path -LiteralPath $_ }
$forbidden = @(
	'ProfilerCaptureV2', 'kProfilerCaptureV2', 'payloadVersion', 'CaptureVersion v2',
	'LoadProfilerCaptureV2', 'SaveProfilerCaptureV2', 'V2Dispatch', 'legacy-v1',
	'legacy-v2', '"TerraEngineProfilerCapture"'
)
$findings = [Collections.Generic.List[string]]::new()
$rootCMake = Join-Path $root 'CMakeLists.txt'
if(Test-Path -LiteralPath $rootCMake -PathType Leaf) {
	$rootText = Get-Content -LiteralPath $rootCMake -Raw
	foreach($token in $forbidden) {
		if($rootText.IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
			$findings.Add("$($rootCMake): contains forbidden profiler legacy token '$token'")
		}
	}
}
foreach($scanRoot in $scanRoots) {
	Get-ChildItem -LiteralPath $scanRoot -Recurse -File -ErrorAction Stop |
		Where-Object { $_.Extension -in @('.cpp','.h','.ps1','.md','.json','.cmake') -or $_.Name -eq 'CMakeLists.txt' } |
		ForEach-Object {
			if([IO.Path]::GetFullPath($_.FullName) -eq [IO.Path]::GetFullPath($PSCommandPath)) { return }
			$text = Get-Content -LiteralPath $_.FullName -Raw
			foreach($token in $forbidden) {
				if($text.IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
					$findings.Add("$($_.FullName): contains forbidden profiler legacy token '$token'")
				}
			}
		}
}
if(-not [string]::IsNullOrWhiteSpace($InstallRoot)) {
	$install = [IO.Path]::GetFullPath($InstallRoot)
	if(-not (Test-Path -LiteralPath $install -PathType Container)) { throw "Install root does not exist: $install" }
	Get-ChildItem -LiteralPath $install -Recurse -File -ErrorAction Stop |
		Where-Object { $_.Extension -in @('.dll','.exe','.h','.cpp','.ps1','.md','.json') } |
		ForEach-Object {
			try { $text = Get-Content -LiteralPath $_.FullName -Raw -ErrorAction Stop } catch { return }
			foreach($token in $forbidden) {
				if($text.IndexOf($token, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
					$findings.Add("$($_.FullName): installed tree contains forbidden profiler legacy token '$token'")
				}
			}
		}
}
if($findings.Count -ne 0) {
	if(-not $Quiet) { $findings | ForEach-Object { Write-Error $_ } }
	throw "Profiler source audit failed with $($findings.Count) finding(s)."
}
if(-not $Quiet) { Write-Output 'Profiler source audit passed: no legacy capture symbols found.' }
