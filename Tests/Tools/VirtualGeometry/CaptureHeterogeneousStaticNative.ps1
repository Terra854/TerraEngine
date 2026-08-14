[CmdletBinding()]
param(
	[string]$Output = 'Tests/Fixtures/RendererPerformanceBaseline/results/final/heterogeneous-static-native-60s.json',
	[string]$BenchmarkExecutable = 'out/build/RelWithDebInfo/Tests/Tools/VirtualGeometry/TerraEngineRendererTopologyBenchmark.exe',
	[ValidateRange(1,86400)][int]$DurationSeconds = 60,
	[ValidateRange(1,600000)][int]$SampleIntervalMilliseconds = 1000
)

# The benchmark executable builds exactly 4,000 deterministic material
# identities and invokes the renderer's topology/grouping API for every timed
# sample. This wrapper only resolves paths and validates the emitted evidence.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
function Resolve-RepositoryPath([string]$Path)
{
	if([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
	return [IO.Path]::GetFullPath((Join-Path $repositoryRoot $Path))
}

$executablePath = Resolve-RepositoryPath $BenchmarkExecutable
$outputPath = Resolve-RepositoryPath $Output
if(-not (Test-Path -LiteralPath $executablePath -PathType Leaf))
{
	throw "Renderer topology benchmark executable missing: $executablePath"
}
& $executablePath --output $outputPath --duration-seconds $DurationSeconds --sample-interval-milliseconds $SampleIntervalMilliseconds
if($LASTEXITCODE -ne 0) { throw "Renderer topology benchmark failed with exit code $LASTEXITCODE" }

$capture = Get-Content -Raw -LiteralPath $outputPath | ConvertFrom-Json
if([UInt64]$capture.workload.renderables -ne 4000 -or
	[UInt64]$capture.workload.distinctMaterialIdentities -ne 4000)
{
	throw 'Renderer topology capture did not preserve exactly 4,000 distinct material identities.'
}
if($capture.environment.route -ne 'renderer-topology-api' -or
	@($capture.rawSamples).Count -eq 0 -or
	[UInt64]$capture.statistics.medianNanoseconds -eq 0)
{
	throw 'Renderer topology capture is missing measured CPU topology/grouping evidence.'
}
foreach($sample in @($capture.rawSamples))
{
	if($sample.availability -ne 'measured' -or
		[UInt64]$sample.distinctMaterialGroupCount -ne 4000 -or
		[UInt64]$sample.singletonOrdinalCount -ne 4000 -or
		[UInt64]$sample.durationNanoseconds -eq 0)
	{
		throw 'Renderer topology capture contains a sample without complete unique-material fallback evidence.'
	}
}
Write-Output "published renderer topology capture with $(@($capture.rawSamples).Count) CPU samples; GPU timing is $($capture.gpuTiming.availability)"
