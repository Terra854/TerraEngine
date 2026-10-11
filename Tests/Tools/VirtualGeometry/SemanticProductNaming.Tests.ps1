[CmdletBinding()]
param([string]$OutputRoot = '')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Run with either bundled pwsh or Windows PowerShell 5.1. Each case copies the
# actual audit into a fresh miniature repository under out, then invokes that
# audit as a child process. No maintained files or full repository scan change.
if([string]::IsNullOrWhiteSpace($OutputRoot))
{
	$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
	$OutputRoot = Join-Path $repository ('out/evidence/semantic-naming-tests/' + [Guid]::NewGuid().ToString('N'))
}
if(Test-Path -LiteralPath $OutputRoot) { throw 'The regression output root must be fresh.' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$hostExecutable = (Get-Process -Id $PID).Path
$configuration = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'semantic-product-naming.json') -Raw | ConvertFrom-Json
$cases = @(
	@{name='all-declared-protocol-literals'; path='Tests/Fixtures/RendererEfficiency/workloads.json'; text='["SC-001","SC-002","SC-003","SC-004","SC-005","SC-006","SC-007","SC-008","SC-009"]'; pass=$true},
	@{name='unknown-criterion'; path='Tests/Fixtures/RendererEfficiency/workloads.json'; text='"SC-010"'; pass=$false},
	@{name='embedded-description'; path='Tests/Fixtures/RendererEfficiency/workloads.json'; text='"Failure for SC-001"'; pass=$false},
	@{name='nested-quoted-description'; path='Tests/Fixtures/RendererEfficiency/workloads.json'; text='"Failure for \"SC-001\""'; pass=$false},
	@{name='multiline-description'; path='Tests/Tools/RendererEfficiency/Compare-RendererEfficiencyEvidence.ps1'; text=('''Failure for' + "`n" + '"SC-001"'''); pass=$false},
	@{name='criterion-filename'; path='Tests/Fixtures/RendererEfficiency/SC-001.json'; text='{}'; pass=$false},
	@{name='outside-declared-file'; path='Engine/ordinary.cpp'; text='"SC-001"'; pass=$false},
	@{name='other-planning-token'; path='Tests/Fixtures/RendererEfficiency/workloads.json'; text='"Spec Kit"'; pass=$false},
	@{name='decoded-protocol-literal'; path='Tests/Fixtures/RendererEfficiency/workloads.json'; text='%22SC-001%22'; pass=$true},
	@{name='decoded-description'; path='Tests/Fixtures/RendererEfficiency/workloads.json'; text='%22Failure%20for%20SC-001%22'; pass=$false},
	@{name='requirement-not-protocol'; path='Tests/Fixtures/RendererEfficiency/workloads.json'; text='"FR-001"'; pass=$false},
	@{name='case-not-protocol'; path='Tests/Fixtures/RendererEfficiency/workloads.json'; text='"sc-001"'; pass=$false},
	@{name='user-agent-instructions'; path='AGENTS.md'; text='Spec Kit /speckit.implement'; pass=$true}
)
$results = [Collections.Generic.List[object]]::new()
foreach($case in $cases)
{
	$caseRoot = Join-Path $OutputRoot $case.name
	foreach($root in $configuration.productRoots) { [IO.Directory]::CreateDirectory((Join-Path $caseRoot $root)) | Out-Null }
	$auditRoot = Join-Path $caseRoot 'Tests/Tools/VirtualGeometry'
	[IO.Directory]::CreateDirectory($auditRoot) | Out-Null
	$audit = Join-Path $auditRoot 'CheckSemanticProductNaming.ps1'
	$config = Join-Path $auditRoot 'semantic-product-naming.json'
	Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CheckSemanticProductNaming.ps1') -Destination $audit
	Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'semantic-product-naming.json') -Destination $config
	$inputPath = Join-Path $caseRoot $case.path
	[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($inputPath)) | Out-Null
	[IO.File]::WriteAllText($inputPath, $case.text, $utf8)
	if($case.name -eq 'all-declared-protocol-literals')
	{
		# Every declared file uses the same narrow rule; no allowance applies to
		# the whole RendererEfficiency directory or to filename checks.
		foreach($relative in $configuration.protocolMetadataAllowance.files)
		{
			$path = Join-Path $caseRoot $relative
			[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
			[IO.File]::WriteAllText($path, $case.text, $utf8)
		}
	}
	$report = Join-Path $caseRoot 'report.json'
	& $hostExecutable -NoProfile -ExecutionPolicy Bypass -File $audit -Configuration $config -ReportPath $report *> (Join-Path $caseRoot 'audit.log')
	$actualExit = $LASTEXITCODE
	$value = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
	$expectedExit = if($case.pass) { 0 } else { 1 }
	if($actualExit -ne $expectedExit -or ($value.status -eq 'passed') -ne $case.pass)
	{
		throw "Case $($case.name) expected exit $expectedExit; actual $actualExit / $($value.status)."
	}
	if(-not $case.pass -and @($value.violations).Count -eq 0) { throw 'A rejected case must retain its actual finding.' }
	$results.Add([ordered]@{name=$case.name;status='Passed';exitCode=$actualExit;findingCount=@($value.violations).Count})
	Write-Output "[PASS] $($case.name)"
}
$summary = [ordered]@{status='Passed';scope='Isolated naming audit regressions only';caseCount=$results.Count;host=$hostExecutable;cases=$results.ToArray()}
[IO.File]::WriteAllText((Join-Path $OutputRoot 'result.json'), ($summary | ConvertTo-Json -Depth 8), $utf8)
Write-Output "$($results.Count)/$($cases.Count) naming audit regressions passed."
