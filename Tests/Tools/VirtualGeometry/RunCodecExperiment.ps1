[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InputMesh,
	[string]$ExperimentExecutable = 'out/build/RelWithDebInfo/Build/Tests/Tools/VirtualGeometry/TerraEngineVirtualGeometryCodecExperiment.exe',
	[string]$OutputDirectory = 'out/evidence/virtual-geometry-production/codec-experiment',
	[int[]]$PageSizesKiB = @(32, 64, 128),
	[switch]$DryRun
)

# This orchestrator never substitutes estimates for codec measurements. It
# validates the experiment matrix, then delegates each page-size run to the
# versioned prototype executable once that executable is available.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
function Resolve-Repo([string]$Path) { if([IO.Path]::IsPathRooted($Path)){return [IO.Path]::GetFullPath($Path)}; return [IO.Path]::GetFullPath((Join-Path $repo $Path)) }
$inputPath = Resolve-Repo $InputMesh
$executablePath = Resolve-Repo $ExperimentExecutable
$outputPath = Resolve-Repo $OutputDirectory
if(-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) { throw "Input mesh is missing: $inputPath" }
if($PageSizesKiB.Count -eq 0 -or @($PageSizesKiB | Where-Object { $_ -notin @(32,64,128) }).Count -gt 0) { throw 'PageSizesKiB must contain only 32, 64, or 128.' }
if(@($PageSizesKiB | Sort-Object -Unique).Count -ne $PageSizesKiB.Count) { throw 'PageSizesKiB entries must be unique.' }
if($DryRun) { Write-Output "validated input and page-size experiment matrix (dry run; no codec was executed): $($PageSizesKiB -join ', ') KiB"; exit 0 }
if(-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) { throw "Codec experiment executable is unavailable: $executablePath" }
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$results = [Collections.Generic.List[object]]::new()
foreach($pageSize in $PageSizesKiB) {
	$resultPath = Join-Path $outputPath ("page-{0}KiB.json" -f $pageSize)
	& $executablePath '--input' $inputPath '--page-size-kib' $pageSize '--output' $resultPath
	if($LASTEXITCODE -ne 0) { throw "Codec experiment failed for ${pageSize}KiB with exit code $LASTEXITCODE" }
	if(-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) { throw "Codec experiment did not publish a result for ${pageSize}KiB" }
	$document = Get-Content -Raw -LiteralPath $resultPath | ConvertFrom-Json
	$measured = @($document.results | Where-Object {
		$_.state -eq 'measured' -and $_.integrityValidated -eq $true -and
		[UInt64]$_.deterministicOutputHash -ne 0
	})
	$offlineOnly = @($document.results | Where-Object {
		$_.codec -eq 'terra' -and $_.state -eq 'unavailable' -and
		$_.availability -eq 'offline-only' -and
		$_.reason -eq 'not-a-production-temf-codec'
	})
	if([int]$document.pageSizeKiB -ne $pageSize -or
		$measured.Count -lt 2 -or $offlineOnly.Count -ne 1)
	{
		throw "Codec experiment for ${pageSize}KiB must publish two integrity-validated production candidates and one explicit offline-only prototype state."
	}
	$results.Add([ordered]@{
		pageSizeKiB = $pageSize
		status = 'measured-result-published'
		executionId = [Guid]::NewGuid().ToString('D')
		artifact = [ordered]@{
			path = $resultPath.Replace($repo + '\','').Replace('\','/')
			bytes = (Get-Item -LiteralPath $resultPath).Length
			sha256 = (Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).
				Hash.ToLowerInvariant()
		}
		measuredCandidates = $measured.Count
		offlineOnlyCandidates = $offlineOnly.Count
	})
}
$manifest = [ordered]@{
	schemaVersion = 2
	status = 'measured-result-published'
	identity = [ordered]@{
		executionId = [Guid]::NewGuid().ToString('D')
		executableSha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).
			Hash.ToLowerInvariant()
	}
	input = [ordered]@{
		path = $inputPath.Replace($repo + '\','').Replace('\','/')
		bytes = (Get-Item -LiteralPath $inputPath).Length
		sha256 = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).
			Hash.ToLowerInvariant()
	}
	experiments = $results
	note = 'Results are executable-produced; unavailable or failed runs are not converted to zeros.'
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath 'experiment-manifest.json') -Encoding utf8
Write-Output "codec experiment manifest written to $outputPath"
