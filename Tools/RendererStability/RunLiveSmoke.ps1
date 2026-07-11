# RunLiveSmoke.ps1 - Opt-in, paced editor/runtime presentation smoke harness.
#
# Example:
#   pwsh Tools/RendererStability/RunLiveSmoke.ps1 -Executable out/install/RelWithDebInfo/TerraEngine-Editor.exe `
#     -Project VirtualizedGeometryTestProject -EvidenceDirectory out/validation/renderer-stability-editor-live `
#     -AllowLiveGpuStress

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $Executable,
	[string] $Project,
	[Parameter(Mandatory = $true)] [string] $EvidenceDirectory,
	[int] $Operations = 50,
	[int] $PaceMilliseconds = 180,
	[switch] $AllowLiveGpuStress
)

$ErrorActionPreference = "Stop"
if (-not $AllowLiveGpuStress) {
	throw "Live GPU stress is opt-in. Pass -AllowLiveGpuStress after deterministic tests pass."
}
if ($Operations -lt 1 -or $Operations -gt 50) {
	throw "Operations must stay within the bounded 1..50 range."
}
if ($PaceMilliseconds -lt 100) {
	throw "PaceMilliseconds must be at least 100 to avoid an unthrottled driver stress loop."
}

$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$evidencePath = [System.IO.Path]::GetFullPath($EvidenceDirectory)
New-Item -ItemType Directory -Force -Path $evidencePath | Out-Null
$startTime = Get-Date
$arguments = @()
if ($Project) {
	$arguments += "--project"
	$arguments += (Resolve-Path -LiteralPath $Project).Path
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

$startParameters = @{ FilePath = $executablePath; PassThru = $true }
if ($arguments.Count -gt 0) { $startParameters.ArgumentList = $arguments }
$process = Start-Process @startParameters
try {
	for ($attempt = 0; $attempt -lt 100 -and $process.MainWindowHandle -eq 0; ++$attempt) {
		Start-Sleep -Milliseconds 100
		$process.Refresh()
		if ($process.HasExited) { throw "Renderer process exited before publishing a window. ExitCode=$($process.ExitCode)" }
	}
	if ($process.MainWindowHandle -eq 0) { throw "Renderer process did not publish a main window within 10 seconds." }

	for ($operation = 0; $operation -lt $Operations; ++$operation) {
		$width = 960 + (($operation % 7) * 37)
		$height = 600 + (($operation % 5) * 31)
		$screen = $screens[$operation % $screens.Count]
		$x = $screen.WorkingArea.Left + 80
		$y = $screen.WorkingArea.Top + 80
		[TerraRendererSmokeWindow]::MoveWindow($process.MainWindowHandle, $x, $y, $width, $height, $true) | Out-Null
		if (($operation % 10) -eq 5) {
			[TerraRendererSmokeWindow]::ShowWindow($process.MainWindowHandle, 6) | Out-Null # minimize
			Start-Sleep -Milliseconds ($PaceMilliseconds * 2)
			[TerraRendererSmokeWindow]::ShowWindow($process.MainWindowHandle, 9) | Out-Null # restore
		}
		Start-Sleep -Milliseconds $PaceMilliseconds
		$process.Refresh()
		if ($process.HasExited) { throw "Renderer process exited during operation $operation. ExitCode=$($process.ExitCode)" }
	}

	$driverEvents = Get-WinEvent -FilterHashtable @{ LogName = "System"; StartTime = $startTime } -ErrorAction SilentlyContinue |
		Where-Object { $_.ProviderName -match "Display|nvlddmkm|amdkmdag|igfx" -or $_.Message -match "driver|device lost|reset" }
	$driverEvents | Format-List TimeCreated, ProviderName, Id, LevelDisplayName, Message |
		Out-File -LiteralPath (Join-Path $evidencePath "driver-events.txt") -Encoding utf8
	if ($driverEvents) { throw "A new display driver event occurred; stopping without further stress." }
	$crashRoots = Get-ChildItem -LiteralPath $env:LOCALAPPDATA -Directory -Filter "TerraEngine*" -ErrorAction SilentlyContinue |
		ForEach-Object { Join-Path $_.FullName "Diagnostics\CrashDumps" } |
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

	@{
		executable = $executablePath
		project = $Project
		operations = $Operations
		paceMilliseconds = $PaceMilliseconds
		displayCount = $screens.Count
		processAlive = -not $process.HasExited
		completedAt = (Get-Date).ToString("o")
	} | ConvertTo-Json | Out-File -LiteralPath (Join-Path $evidencePath "summary.json") -Encoding utf8
}
finally {
	if (-not $process.HasExited) {
		$process.CloseMainWindow() | Out-Null
		if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id }
	}
}
