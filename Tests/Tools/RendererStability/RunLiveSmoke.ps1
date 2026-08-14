# RunLiveSmoke.ps1 - Opt-in, paced editor/runtime presentation smoke harness.
#
# Example:
#   pwsh Tests/Tools/RendererStability/RunLiveSmoke.ps1 -Executable out/install/RelWithDebInfo/TerraEngine-Editor.exe `
#     -Project VirtualizedGeometryTestProject -EvidenceDirectory out/validation/renderer-stability-editor-live `
#     -AllowLiveGpuStress

[CmdletBinding(DefaultParameterSetName = "Live")]
param(
	[Parameter(Mandatory = $true, ParameterSetName = "Live")] [string] $Executable,
	[Parameter(ParameterSetName = "Live")] [string] $Project,
	[Parameter(Mandatory = $true, ParameterSetName = "Live")] [string] $EvidenceDirectory,
	[Parameter(ParameterSetName = "Live")] [int] $Operations = 50,
	[Parameter(ParameterSetName = "Live")] [int] $PaceMilliseconds = 180,
	[Parameter(ParameterSetName = "Live")] [switch] $AllowLiveGpuStress,
	# Test callers can provide a deterministic System-log query. It receives "baseline"
	# or "since" plus the baseline time and must return event-like objects.
	[Parameter(ParameterSetName = "Live")] [scriptblock] $SystemEventQuery,
	[Parameter(Mandatory = $true, ParameterSetName = "HarnessTests")] [switch] $RunHarnessTests
)

$ErrorActionPreference = "Stop"

function ConvertTo-TerraRendererSmokeEventEvidence {
	param([Parameter(Mandatory = $true)] $Event)

	# Keep evidence plain and serializable: the live EventRecord object owns native
	# resources and is unsuitable for a durable fail-closed report.
	[ordered]@{
		recordId = $Event.RecordId
		timeCreated = if ($null -ne $Event.TimeCreated) { ([datetime] $Event.TimeCreated).ToString("o") } else { $null }
		provider = [string] $Event.ProviderName
		id = $Event.Id
		level = [string] $Event.LevelDisplayName
		message = [string] $Event.Message
	}
}

function Get-TerraRendererSmokeSystemBaseline {
	param([Parameter(Mandatory = $true)] [scriptblock] $Query)

	$baseline = @(& $Query "baseline" ([datetime]::MinValue) | Select-Object -First 1)
	if ($baseline.Count -ne 1 -or $null -eq $baseline[0].TimeCreated -or $null -eq $baseline[0].RecordId) {
		throw "Could not establish the required Windows System event RecordId/time baseline before launch."
	}
	return $baseline[0]
}

function Test-TerraRendererSmokeQualifyingEvent {
	param([Parameter(Mandatory = $true)] $Event)

	# Keep the established display-driver predicate stable so comparison runs use the
	# same event policy as the original renderer-stability baseline.
	$provider = [string] $Event.ProviderName
	$message = [string] $Event.Message
	return $provider -match "Display|nvlddmkm|amdkmdag|igfx" -or $message -match "driver|device lost|reset"
}

function Test-TerraRendererSmokeEventAfterBaseline {
	param(
		[Parameter(Mandatory = $true)] $Event,
		[Parameter(Mandatory = $true)] $Baseline
	)

	$baselineTime = [datetime] $Baseline.TimeCreated
	$eventTime = if ($null -ne $Event.TimeCreated) { [datetime] $Event.TimeCreated } else { $null }
	$baselineRecordId = [Int64] $Baseline.RecordId
	$eventRecordId = if ($null -ne $Event.RecordId) { [Int64] $Event.RecordId } else { $null }

	# Record IDs establish ordering while the System log is continuous. If Windows
	# clears or replaces the log during a run, a later timestamp with a reset/lower
	# RecordId must fail closed rather than hiding a new graphics event.
	if ($null -ne $eventRecordId) {
		return $null -ne $eventTime -and (
			($eventRecordId -gt $baselineRecordId -and $eventTime -ge $baselineTime) -or
			($eventRecordId -le $baselineRecordId -and $eventTime -gt $baselineTime))
	}
	return $null -ne $eventTime -and $eventTime -gt $baselineTime
}

function Get-TerraRendererSmokeNewDriverEvents {
	param(
		[Parameter(Mandatory = $true)] $Baseline,
		[Parameter(Mandatory = $true)] [scriptblock] $Query
	)

	# Query from the frozen baseline time on every poll. The RecordId predicate
	# below removes pre-existing same-window events rather than relying on time alone.
	return @(& $Query "since" ([datetime] $Baseline.TimeCreated) |
		Where-Object { Test-TerraRendererSmokeQualifyingEvent $_ } |
		Where-Object { Test-TerraRendererSmokeEventAfterBaseline -Event $_ -Baseline $Baseline })
}

function Stop-TerraRendererSmokeProcessTree {
	param([Parameter(Mandatory = $true)] [int] $ProcessId)

	# taskkill /T targets the launched editor and descendants together. This is more
	# reliable than closing only the UI process when a renderer helper is still alive.
	$taskKill = Join-Path $env:SystemRoot "System32\\taskkill.exe"
	if (Test-Path -LiteralPath $taskKill) {
		& $taskKill /PID $ProcessId /T /F | Out-Null
		if ($LASTEXITCODE -eq 0) { return }
	}
	# The parent can already have exited by the time the event poll runs. The fallback
	# keeps cleanup best-effort while the gate still reports the fail-closed evidence.
	Stop-Process -Id $ProcessId -Force -ErrorAction SilentlyContinue
}

function Invoke-TerraRendererSmokeDriverEventGate {
	param(
		[Parameter(Mandatory = $true)] $Baseline,
		[Parameter(Mandatory = $true)] [scriptblock] $Query,
		[Parameter(Mandatory = $true)] [int] $ProcessId,
		[Parameter(Mandatory = $true)] [string] $EvidencePath,
		[Parameter(Mandatory = $true)] [int] $Operation,
		[Parameter(Mandatory = $true)] [string] $Stage,
		[scriptblock] $StopProcessTree = ${function:Stop-TerraRendererSmokeProcessTree}
	)

	$newEvents = @(Get-TerraRendererSmokeNewDriverEvents -Baseline $Baseline -Query $Query)
	if ($newEvents.Count -eq 0) { return }

	$occurredAt = Get-Date
	$failure = [ordered]@{
		outcome = "fail-closed"
		operation = $Operation
		stage = $Stage
		detectedAt = $occurredAt.ToString("o")
		baseline = ConvertTo-TerraRendererSmokeEventEvidence $Baseline
		events = @($newEvents | ForEach-Object { ConvertTo-TerraRendererSmokeEventEvidence $_ })
		processId = $ProcessId
	}
	$failure | ConvertTo-Json -Depth 6 | Out-File -LiteralPath (Join-Path $EvidencePath "driver-event-failure.json") -Encoding utf8
	$failure.events | Format-List recordId, timeCreated, provider, id, level, message |
		Out-File -LiteralPath (Join-Path $EvidencePath "driver-events.txt") -Encoding utf8

	# Stop before throwing: callers cannot accidentally proceed to operation N + 1.
	& $StopProcessTree $ProcessId
	throw "A new qualifying display/driver/graphics event was detected at operation $Operation ($Stage) at $($failure.detectedAt); the launched editor process tree was stopped fail-closed."
}

function Wait-TerraRendererSmokeWithDriverPolling {
	param(
		[Parameter(Mandatory = $true)] [System.Diagnostics.Process] $Process,
		[Parameter(Mandatory = $true)] [int] $Milliseconds,
		[Parameter(Mandatory = $true)] $Baseline,
		[Parameter(Mandatory = $true)] [scriptblock] $Query,
		[Parameter(Mandatory = $true)] [string] $EvidencePath,
		[Parameter(Mandatory = $true)] [int] $Operation,
		[Parameter(Mandatory = $true)] [string] $Stage
	)

	# Poll at 100 ms intervals instead of sleeping through minimize/restore delays,
	# so a fault aborts the active operation rather than the next operation.
	$remaining = $Milliseconds
	while ($remaining -gt 0) {
		$wait = [Math]::Min(100, $remaining)
		Start-Sleep -Milliseconds $wait
		$remaining -= $wait
		$Process.Refresh()
		if ($Process.HasExited) { throw "Renderer process exited during operation $Operation ($Stage). ExitCode=$($Process.ExitCode)" }
		Invoke-TerraRendererSmokeDriverEventGate -Baseline $Baseline -Query $Query -ProcessId $Process.Id -EvidencePath $EvidencePath -Operation $Operation -Stage $Stage
	}
}

function ConvertFrom-TerraRendererSmokePresentationMarker {
	param([Parameter(Mandatory = $true)] [string] $Text)

	$match = [regex]::Match($Text, '\[Vulkan\] Editor presentation ready generation=(\d+) extent=(\d+)x(\d+)')
	if (-not $match.Success) { return $null }
	return [ordered]@{
		status = "observed"
		marker = $match.Value
		generation = [UInt64] $match.Groups[1].Value
		width = [UInt32] $match.Groups[2].Value
		height = [UInt32] $match.Groups[3].Value
		source = "editor.stdout.log"
	}
}

function Wait-TerraRendererSmokePresentationReady {
	param(
		[Parameter(Mandatory = $true)] [System.Diagnostics.Process] $Process,
		[Parameter(Mandatory = $true)] [string] $StdoutPath,
		[Parameter(Mandatory = $true)] $Baseline,
		[Parameter(Mandatory = $true)] [scriptblock] $Query,
		[Parameter(Mandatory = $true)] [string] $EvidencePath,
		[ValidateRange(1, 120)] [int] $TimeoutSeconds = 30
	)

	# A native window can exist before Vulkan has presented anything. Wait for the
	# backend's first-successful-present marker while retaining the same fail-fast
	# System-event polling used by the resize loop.
	$deadline = [datetime]::UtcNow.AddSeconds($TimeoutSeconds)
	do {
		$Process.Refresh()
		if ($Process.HasExited) {
			throw "Renderer process exited before the first successful editor presentation. ExitCode=$($Process.ExitCode)"
		}
		Invoke-TerraRendererSmokeDriverEventGate -Baseline $Baseline -Query $Query -ProcessId $Process.Id -EvidencePath $EvidencePath -Operation -1 -Stage "presentation-readiness"
		$text = if (Test-Path -LiteralPath $StdoutPath -PathType Leaf) {
			Get-Content -Raw -LiteralPath $StdoutPath -ErrorAction SilentlyContinue
		} else { "" }
		$readiness = ConvertFrom-TerraRendererSmokePresentationMarker -Text ([string] $text)
		if ($null -ne $readiness) {
			$readiness["observedAt"] = (Get-Date).ToString("o")
			$readiness | ConvertTo-Json -Depth 3 | Out-File -LiteralPath (Join-Path $EvidencePath "presentation-readiness.json") -Encoding utf8
			return $readiness
		}
		Start-Sleep -Milliseconds 100
	} while ([datetime]::UtcNow -lt $deadline)

	throw "Renderer did not publish the first-successful-present marker within $TimeoutSeconds seconds."
}

function Get-TerraRendererSmokeSelectedGpuNames {
	param([Parameter(Mandatory = $true)] [string] $Text)

	# PowerShell does not treat a backslash as a string escape. Use one regex
	# backslash for whitespace and ANSI control sequences, then remove only the
	# terminal color suffix so the Vulkan device name remains exact evidence.
	return @([regex]::Matches($Text, 'Selected GPU:\s*([^\r\n]+)') |
		ForEach-Object {
			[regex]::Replace($_.Groups[1].Value, '\x1B\[[0-9;]*m', '').Trim()
		} |
		Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
		Select-Object -Unique)
}

function Invoke-TerraRendererSmokeHarnessTests {
	$testRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("terra-renderer-smoke-tests-" + [guid]::NewGuid().ToString("N"))
	New-Item -ItemType Directory -Path $testRoot | Out-Null
	try {
		$baselineTime = [datetime]::UtcNow
		$baseline = [pscustomobject]@{ RecordId = 100; TimeCreated = $baselineTime; ProviderName = "Service Control Manager"; Id = 1; LevelDisplayName = "Information"; Message = "Existing event" }
		$preexisting = [pscustomobject]@{ RecordId = 99; TimeCreated = $baselineTime.AddSeconds(-1); ProviderName = "Display"; Id = 13; LevelDisplayName = "Error"; Message = "Graphics FECS Exception" }
		$noFailureQuery = { param($kind, $since) if ($kind -eq "baseline") { return $baseline }; if ($kind -eq "since") { return $preexisting } }
		$observedBaseline = Get-TerraRendererSmokeSystemBaseline -Query $noFailureQuery
		if ($observedBaseline.RecordId -ne 100) { throw "The injected event-query seam did not provide the baseline record." }
		$script:preexistingStopCount = 0
		Invoke-TerraRendererSmokeDriverEventGate -Baseline $baseline -Query $noFailureQuery -ProcessId 4242 -EvidencePath $testRoot -Operation 0 -Stage "after-resize" -StopProcessTree { param($id) $script:preexistingStopCount += 1 }
		if ($script:preexistingStopCount -ne 0) { throw "Pre-existing System events must not stop a live smoke run." }
		$resetRecord = [pscustomobject]@{ RecordId = 1; TimeCreated = $baselineTime.AddSeconds(1); ProviderName = "Display"; Id = 13; LevelDisplayName = "Error"; Message = "Graphics event after a System-log reset" }
		if (-not (Test-TerraRendererSmokeEventAfterBaseline -Event $resetRecord -Baseline $baseline)) {
			throw "A qualifying event after a System-log RecordId reset must fail closed."
		}
		$readiness = ConvertFrom-TerraRendererSmokePresentationMarker -Text "prefix [Vulkan] Editor presentation ready generation=7 extent=1600x900 suffix"
		if ($null -eq $readiness -or $readiness.generation -ne 7 -or $readiness.width -ne 1600 -or $readiness.height -ne 900) {
			throw "The first-successful-present marker must produce typed readiness evidence."
		}
		$ansiEscape = [char] 27
		$gpuOutput = "${ansiEscape}[97m[DEBUG] Selected GPU: NVIDIA GeForce RTX 4080 Laptop GPU${ansiEscape}[0m"
		$selectedGpuNames = @(Get-TerraRendererSmokeSelectedGpuNames -Text $gpuOutput)
		if ($selectedGpuNames.Count -ne 1 -or $selectedGpuNames[0] -ne "NVIDIA GeForce RTX 4080 Laptop GPU") {
			throw "ANSI-colored Vulkan output must produce the exact selected GPU name."
		}

		$operations = @()
		$script:currentOperation = -1
		$script:stopCount = 0
		$newEvent = [pscustomobject]@{ RecordId = 101; TimeCreated = $baselineTime.AddSeconds(2); ProviderName = "nvlddmkm"; Id = 13; LevelDisplayName = "Error"; Message = "Graphics exception" }
		$eventAtTwoQuery = { param($kind, $since) if ($kind -eq "since" -and $script:currentOperation -eq 2) { return $newEvent } }
		for ($operation = 0; $operation -lt 5; ++$operation) {
			$operations += $operation
			$script:currentOperation = $operation
			try {
				Invoke-TerraRendererSmokeDriverEventGate -Baseline $baseline -Query $eventAtTwoQuery -ProcessId 4242 -EvidencePath $testRoot -Operation $operation -Stage "after-resize" -StopProcessTree { param($id) $script:stopCount += 1 }
			}
			catch {
				if ($operation -ne 2) { throw }
				break
			}
		}
		if (($operations -join ",") -ne "0,1,2" -or $script:stopCount -ne 1) {
			throw "A driver event at operation N must stop the process tree and prevent operation N + 1."
		}
		Write-Output "RunLiveSmoke harness tests passed: pre-existing events ignored; new event at N stopped before N+1."
	}
	finally {
		Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
	}
}

if ($RunHarnessTests) {
	Invoke-TerraRendererSmokeHarnessTests
	return
}

if (-not $AllowLiveGpuStress) {
	throw "Live GPU stress is opt-in. Pass -AllowLiveGpuStress after deterministic tests pass."
}
if ($Operations -lt 1 -or $Operations -gt 50) {
	throw "Operations must stay within the bounded 1..50 range."
}
if ($PaceMilliseconds -lt 100) {
	throw "PaceMilliseconds must be at least 100 to avoid an unthrottled driver stress loop."
}
if ($null -eq $SystemEventQuery) {
	$SystemEventQuery = {
		param([string] $Mode, [datetime] $Since)
		if ($Mode -eq "baseline") {
			Get-WinEvent -LogName "System" -MaxEvents 1 -ErrorAction Stop
		}
		else {
			Get-WinEvent -FilterHashtable @{ LogName = "System"; StartTime = $Since } -ErrorAction Stop
		}
	}
}

$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$evidencePath = [System.IO.Path]::GetFullPath($EvidenceDirectory)
if (Test-Path -LiteralPath $evidencePath) {
	if (-not (Test-Path -LiteralPath $evidencePath -PathType Container) -or @(Get-ChildItem -LiteralPath $evidencePath -Force).Count -gt 0) {
		throw "EvidenceDirectory must be a fresh, empty directory: '$evidencePath'."
	}
}
New-Item -ItemType Directory -Force -Path $evidencePath | Out-Null
$stdoutPath = Join-Path $evidencePath "editor.stdout.log"
$stderrPath = Join-Path $evidencePath "editor.stderr.log"

# Freeze a System-log RecordId/time before launch. Every later poll compares against
# this snapshot so a pre-existing driver event cannot invalidate a fresh sample.
$systemEventBaseline = Get-TerraRendererSmokeSystemBaseline -Query $SystemEventQuery
(ConvertTo-TerraRendererSmokeEventEvidence $systemEventBaseline) |
	ConvertTo-Json -Depth 3 | Out-File -LiteralPath (Join-Path $evidencePath "system-event-baseline.json") -Encoding utf8
$startTime = Get-Date
$arguments = @()
if ($Project) {
	$arguments += "--project"
	$arguments += (Resolve-Path -LiteralPath $Project).Path
}

$workerCountText = [Environment]::GetEnvironmentVariable("TERRAENGINE_THREADPOOL_WORKERS", "Process")
$workerCount = 0
if (-not [int]::TryParse($workerCountText, [ref] $workerCount) -or $workerCount -ne 31) {
	throw "Renderer smoke requires TERRAENGINE_THREADPOOL_WORKERS=31; observed '$workerCountText'."
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class TerraRendererSmokeWindow {
	[DllImport("user32.dll", SetLastError=true)] public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int width, int height, bool repaint);
	[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int command);
}
"@
Add-Type -AssemblyName System.Windows.Forms
$screens = [System.Windows.Forms.Screen]::AllScreens

$startParameters = @{
	FilePath = $executablePath
	PassThru = $true
	RedirectStandardOutput = $stdoutPath
	RedirectStandardError = $stderrPath
}
if ($arguments.Count -gt 0) { $startParameters.ArgumentList = $arguments }
$process = Start-Process @startParameters
$resizeOperations = @()
$presentationReadiness = $null
try {
	for ($attempt = 0; $attempt -lt 100 -and $process.MainWindowHandle -eq 0; ++$attempt) {
		Start-Sleep -Milliseconds 100
		$process.Refresh()
		if ($process.HasExited) { throw "Renderer process exited before publishing a window. ExitCode=$($process.ExitCode)" }
		Invoke-TerraRendererSmokeDriverEventGate -Baseline $systemEventBaseline -Query $SystemEventQuery -ProcessId $process.Id -EvidencePath $evidencePath -Operation -1 -Stage "window-startup"
	}
	if ($process.MainWindowHandle -eq 0) { throw "Renderer process did not publish a main window within 10 seconds." }
	$presentationReadiness = Wait-TerraRendererSmokePresentationReady -Process $process -StdoutPath $stdoutPath -Baseline $systemEventBaseline -Query $SystemEventQuery -EvidencePath $evidencePath

	for ($operation = 0; $operation -lt $Operations; ++$operation) {
		$width = 960 + (($operation % 7) * 37)
		$height = 600 + (($operation % 5) * 31)
		$screen = $screens[$operation % $screens.Count]
		$x = $screen.WorkingArea.Left + 80
		$y = $screen.WorkingArea.Top + 80
		if (-not [TerraRendererSmokeWindow]::MoveWindow($process.MainWindowHandle, $x, $y, $width, $height, $true)) {
			throw "MoveWindow failed during renderer smoke operation $operation."
		}
		# Persist every requested size so the evidence converter can reject a shortened or altered workload.
		$resizeOperations += [ordered]@{
			operation = $operation
			width = $width
			height = $height
			displayIndex = $operation % $screens.Count
			x = $x
			y = $y
		}
		Invoke-TerraRendererSmokeDriverEventGate -Baseline $systemEventBaseline -Query $SystemEventQuery -ProcessId $process.Id -EvidencePath $evidencePath -Operation $operation -Stage "after-resize"
		if (($operation % 10) -eq 5) {
			[TerraRendererSmokeWindow]::ShowWindow($process.MainWindowHandle, 6) | Out-Null # minimize
			Wait-TerraRendererSmokeWithDriverPolling -Process $process -Milliseconds ($PaceMilliseconds * 2) -Baseline $systemEventBaseline -Query $SystemEventQuery -EvidencePath $evidencePath -Operation $operation -Stage "minimize-wait"
			[TerraRendererSmokeWindow]::ShowWindow($process.MainWindowHandle, 9) | Out-Null # restore
			Invoke-TerraRendererSmokeDriverEventGate -Baseline $systemEventBaseline -Query $SystemEventQuery -ProcessId $process.Id -EvidencePath $evidencePath -Operation $operation -Stage "after-restore"
		}
		Wait-TerraRendererSmokeWithDriverPolling -Process $process -Milliseconds $PaceMilliseconds -Baseline $systemEventBaseline -Query $SystemEventQuery -EvidencePath $evidencePath -Operation $operation -Stage "operation-wait"
	}

	# Close the final event window through the same stop-before-throw gate used by
	# each operation. A fault arriving just after the last paced wait still cannot
	# leave the launched editor process tree running.
	Invoke-TerraRendererSmokeDriverEventGate -Baseline $systemEventBaseline -Query $SystemEventQuery -ProcessId $process.Id -EvidencePath $evidencePath -Operation $Operations -Stage "post-workload"
	$driverEvents = @()
	New-Item -ItemType File -Force -Path (Join-Path $evidencePath "driver-events.txt") | Out-Null
	$crashRoots = Get-ChildItem -LiteralPath $env:LOCALAPPDATA -Directory -Filter "TerraEngine*" -ErrorAction SilentlyContinue |
		ForEach-Object { Join-Path $_.FullName "Diagnostics\\CrashDumps" } |
		Where-Object { Test-Path -LiteralPath $_ }
	$newCrashReports = foreach ($root in $crashRoots) {
		Get-ChildItem -LiteralPath $root -File -ErrorAction SilentlyContinue |
			Where-Object { $_.LastWriteTime -ge $startTime }
	}
	$newCrashReports | Select-Object FullName, Length, LastWriteTime |
		Format-Table -AutoSize | Out-File -LiteralPath (Join-Path $evidencePath "crash-reports.txt") -Encoding utf8
	if ($newCrashReports) { throw "A new renderer crash report was created; stopping without further stress." }
	$diagnosticRoots = Get-ChildItem -LiteralPath $env:LOCALAPPDATA -Directory -Filter "TerraEngine*" -ErrorAction SilentlyContinue
	$newLogs = foreach ($root in $diagnosticRoots) {
		Get-ChildItem -LiteralPath $root.FullName -File -Recurse -ErrorAction SilentlyContinue |
			Where-Object { $_.LastWriteTime -ge $startTime -and $_.Extension -in @(".log", ".txt") }
	}
	$newLogs | Select-Object FullName, Length, LastWriteTime |
		Format-Table -AutoSize | Out-File -LiteralPath (Join-Path $evidencePath "diagnostic-logs.txt") -Encoding utf8

	$process.Refresh()
	if ($process.HasExited) {
		throw "Renderer process exited before the smoke harness requested shutdown. ExitCode=$($process.ExitCode)"
	}
	$processAliveDuringStress = $true
	$process.CloseMainWindow() | Out-Null
	if (-not $process.WaitForExit(10000)) {
		Stop-TerraRendererSmokeProcessTree -ProcessId $process.Id
		$process.WaitForExit(5000) | Out-Null
		throw "Renderer process did not close within the bounded 10 second shutdown window."
	}

	# Vulkan initialization writes the selected physical device to the captured editor output.
	$combinedOutput = ((Get-Content -Raw -LiteralPath $stdoutPath -ErrorAction SilentlyContinue) + "`n" +
		(Get-Content -Raw -LiteralPath $stderrPath -ErrorAction SilentlyContinue))
	$selectedGpuNames = @(Get-TerraRendererSmokeSelectedGpuNames -Text $combinedOutput)
	if ($selectedGpuNames.Count -ne 1) {
		throw "Expected exactly one Vulkan 'Selected GPU' record; observed $($selectedGpuNames.Count)."
	}
	$selectedGpu = $selectedGpuNames[0]
	$expectedGpu = "NVIDIA GeForce RTX 4080 Laptop GPU"
	if ($selectedGpu -ne $expectedGpu) {
		throw "Renderer selected '$selectedGpu'; the locked comparison identity requires '$expectedGpu'."
	}
	if ($resizeOperations.Count -ne $Operations) {
		throw "Renderer smoke recorded $($resizeOperations.Count) resize operations; expected $Operations."
	}
	for ($operation = 0; $operation -lt $Operations; ++$operation) {
		$expectedWidth = 960 + (($operation % 7) * 37)
		$expectedHeight = 600 + (($operation % 5) * 31)
		$record = $resizeOperations[$operation]
		if ($record.operation -ne $operation -or $record.width -ne $expectedWidth -or $record.height -ne $expectedHeight) {
			throw "Renderer resize record $operation does not match the frozen workload formula."
		}
	}

	[ordered]@{
		executable = $executablePath
		project = $Project
		graphicsBackend = "Vulkan"
		selectedGpu = $selectedGpu
		discreteGpuSelectedExplicitly = $true
		workerCount = $workerCount
		operations = $Operations
		paceMilliseconds = $PaceMilliseconds
		windowWidths = "960 + (operation mod 7) * 37"
		windowHeights = "600 + (operation mod 5) * 31"
		resizeOperations = $resizeOperations
		displayCount = $screens.Count
		processAlive = $processAliveDuringStress
		driverEvents = @($driverEvents).Count
		crashReports = @($newCrashReports).Count
		presentationReadiness = $presentationReadiness
		systemEventBaseline = ConvertTo-TerraRendererSmokeEventEvidence $systemEventBaseline
		stdout = $stdoutPath
		stderr = $stderrPath
		completedAt = (Get-Date).ToString("o")
	} | ConvertTo-Json -Depth 8 | Out-File -LiteralPath (Join-Path $evidencePath "summary.json") -Encoding utf8
}
finally {
	if ($null -ne $process -and -not $process.HasExited) {
		$process.CloseMainWindow() | Out-Null
		if (-not $process.WaitForExit(5000)) {
			# Cleanup is bounded so a failed run cannot leave an installed editor contaminating later samples.
			Stop-TerraRendererSmokeProcessTree -ProcessId $process.Id
			$process.WaitForExit(5000) | Out-Null
		}
	}
}
