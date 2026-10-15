<#
.SYNOPSIS
Run the installed real-allocation fixture with a process-tree watchdog.
.DESCRIPTION
Configure BUILD_TESTING, build/install the fixture and pass its executable. Use
a fresh log directory. Every lane must produce exactly Repetitions receipts.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$Executable,
	[ValidateRange(1,10000)][int]$Repetitions=100,
	[string]$LogDirectory='',
	[string]$WatchdogPath=(Join-Path $PSScriptRoot '../Correctness/RunWithWatchdog.ps1')
)
$ErrorActionPreference='Stop'
if([string]::IsNullOrWhiteSpace($LogDirectory)) {
	$LogDirectory=Join-Path ([IO.Path]::GetTempPath()) ('TerraEngine-render-allocation-'+[Guid]::NewGuid().ToString('N'))
}
& $WatchdogPath -Executable $Executable -ArgumentList @([string]$Repetitions) -TimeoutSeconds 120 -LogDirectory $LogDirectory
if($LASTEXITCODE -ne 0) { throw "Allocation child failed with exit $LASTEXITCODE; evidence: $LogDirectory" }
$output=[IO.File]::ReadAllLines((Join-Path $LogDirectory 'stdout.log'))
$records=@($output | Where-Object { $_.StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json })
$lanes=@('rejection','scratch','receipt-success','receipt-rejection','seal-null','seal-rejected','seal-valid','publish-success','publish-session','publish-stale')
$outcomes=@{'rejection'=9;'scratch'=2;'receipt-success'=0;'receipt-rejection'=9;'seal-null'=4;'seal-rejected'=9;'seal-valid'=0;'publish-success'=0;'publish-session'=9;'publish-stale'=4}
if($records.Count -ne $Repetitions*$lanes.Count) { throw 'Missing or extra allocation checkpoint receipts.' }
foreach($lane in $lanes) {
	$selected=@($records | Where-Object lane -eq $lane)
	if($selected.Count -ne $Repetitions -or @($selected.iteration | Sort-Object -Unique).Count -ne $Repetitions) { throw "Incomplete lane $lane" }
	foreach($row in $selected) {
		foreach($field in @('lane','iteration','attempts','failedAllocations','rootBefore','rootAfter','outcome','healthyFollowup')) {
			if($null -eq $row.PSObject.Properties[$field]) { throw "Missing field $field in $lane" }
		}
		if($row.iteration -lt 0 -or $row.iteration -ge $Repetitions -or !$row.healthyFollowup) { throw "Invalid checkpoint in $lane" }
		if($row.outcome -ne $outcomes[$lane]) { throw "Wrong publication outcome in $lane" }
		if($lane -eq 'scratch') {
			if($row.attempts -lt 1 -or $row.failedAllocations -lt 1 -or $row.rootBefore -ne $row.rootAfter -or $row.outcome -ne 2) { throw 'Scratch did not prove a real atomic allocation failure.' }
		} elseif($row.attempts -ne 0 -or $row.failedAllocations -ne 0) { throw "Unexpected allocation in $lane" }
		if($lane -in @('publish-session','publish-stale') -and $row.rootBefore -ne $row.rootAfter) { throw 'Rejected exchange changed the root.' }
	}
}
if(-not ($output -match ('^allocation safety passed: repetitions='+$Repetitions+' checkpoints='+($Repetitions*10)+' normal_exit=1$'))) { throw 'Missing normal child completion.' }
@{schemaVersion=1;repetitions=$Repetitions;checkpoints=$records.Count;normalExit=$true;lanes=$lanes;records=$records} |
	ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $LogDirectory 'allocation-safety.json') -Encoding UTF8
Write-Output "renderer allocation safety passed: $($records.Count) checkpoints; real scratch faults and normal child exit"
