#requires -Version 7.0
<#
Build the same production ownership controller and counted tests in three fresh,
isolated directories. This is source/toolchain qualification, not GPU acceptance.
Prerequisites: Windows, MSVC C++20 and its matching vcvars64 environment script.
Example (use an unused WorkRoot for every invocation):
pwsh -File Tests/Tools/ReadbackLifetime/TestReadbackLifetimeSensitivity.ps1 `
  -SourceRoot . -WorkRoot out/validation/runtime-contract-quality/readback-mutant/run-001 `
  -CompilerPath '<MSVC>/bin/Hostx64/x64/cl.exe' -VcVarsPath '<VS>/VC/Auxiliary/Build/vcvars64.bat'
No shared source, build cache or installed binary is modified or deleted.
#>
[CmdletBinding()]
param(
	[string]$SourceRoot = (Join-Path $PSScriptRoot '../../..'),
	[Parameter(Mandatory)][string]$WorkRoot,
	[Parameter(Mandatory)][string]$CompilerPath,
	[string]$VcVarsPath,
	[ValidateRange(1, 600)][int]$CompileTimeoutSeconds = 120,
	[ValidateRange(1, 120)][int]$TestTimeoutSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows) { throw 'This MSVC source-sensitivity runner requires Windows.' }
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$CompilerPath = (Resolve-Path -LiteralPath $CompilerPath).Path
if ($VcVarsPath) { $VcVarsPath = (Resolve-Path -LiteralPath $VcVarsPath).Path }
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $WorkRoot) { throw 'WorkRoot must be fresh and nonexistent; existing evidence is never overwritten.' }
$comparison = [StringComparison]::OrdinalIgnoreCase
if ($WorkRoot.Equals($SourceRoot, $comparison) -or $SourceRoot.StartsWith($WorkRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, $comparison)) {
	throw 'WorkRoot cannot equal or contain SourceRoot.'
}
foreach ($protected in @('Engine', 'Tests', 'External', '.git')) {
	$protectedRoot = Join-Path $SourceRoot $protected
	if ($WorkRoot.Equals($protectedRoot, $comparison) -or $WorkRoot.StartsWith($protectedRoot + [IO.Path]::DirectorySeparatorChar, $comparison)) {
		throw 'WorkRoot cannot be inside source, dependency or Git directories.'
	}
}
# cmd is needed only to load the matching compiler environment. Reject shell
# expansion characters rather than interpreting surprising paths as commands.
foreach ($path in @($CompilerPath, $VcVarsPath, $WorkRoot)) {
	if ($path -and $path -match '[\r\n"&|<>^%!]' ) { throw 'Tool/work paths contain unsupported shell metacharacters.' }
}
New-Item -ItemType Directory -Path $WorkRoot | Out-Null
$receiptPath = Join-Path $WorkRoot 'sensitivity.json'
$relativeFiles = @(
	'Engine/Renderer/RHI/Vulkan/VulkanReadbackLifecycle.h',
	'Engine/Renderer/RHI/Vulkan/VulkanReadbackLifecycle.cpp',
	'Engine/Core/Export.h',
	'Tests/Engine/Renderer/VulkanReadbackLifetimeTests.cpp',
	'Tests/Engine/TestHarness.h'
)
$controllerPath = 'Engine/Renderer/RHI/Vulkan/VulkanReadbackLifecycle.cpp'
$expectedCase = 'Renderer.ReadbackLifetime.BeforeDeadlineRetainsAllOwners'
$expectedAssertion = "[FAIL] ${expectedCase}: staging buffer retirement count (expected '0', got '1')"
$flags = @('/nologo', '/Bv', '/std:c++20', '/EHsc', '/W4', '/O2', '/MT', '/DNDEBUG', '/permissive-',
	'/Zc:__cplusplus', '/D_CRT_SECURE_NO_WARNINGS', '/DTERRAENGINE_ENGINE_BUILD', '/DTERRAENGINE_READBACK_LIFETIME_STANDALONE')
$receipt = [ordered]@{
	schemaVersion = 1; evidenceKind = 'mutation-sensitivity'; status = 'incomplete'; realGpuEvidence = $false
	startedUtc = [DateTime]::UtcNow.ToString('o'); sourceRoot = $SourceRoot; workRoot = $WorkRoot
	compilerPath = $CompilerPath; compilerSha256 = (Get-FileHash -LiteralPath $CompilerPath -Algorithm SHA256).Hash
	compilerVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($CompilerPath).FileVersion
	vcVarsPath = $VcVarsPath; flags = $flags; expectedCase = $expectedCase; expectedAssertion = $expectedAssertion
	scriptSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
	sourceManifest = @(); mutation = $null; variants = @(); sharedSourcesUnchanged = $false; failure = $null
}

function Save-Receipt {
	$receipt | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath $receiptPath -Encoding utf8NoBOM
}
function Check-SharedSources {
	foreach ($entry in $receipt.sourceManifest) {
		$hash = (Get-FileHash -LiteralPath (Join-Path $SourceRoot $entry.path) -Algorithm SHA256).Hash
		if ($hash -ne $entry.sha256) { throw "Shared source drifted during sensitivity run: $($entry.path)" }
	}
}
function Copy-Closure([string]$destination) {
	foreach ($entry in $receipt.sourceManifest) {
		$target = Join-Path $destination $entry.path
		New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
		[IO.File]::Copy((Join-Path $SourceRoot $entry.path), $target, $false)
		if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $entry.sha256) { throw 'Copied source hash mismatch.' }
	}
}
function Run-Process([string]$file, [string[]]$arguments, [string]$directory, [string]$logStem, [int]$seconds) {
	$info = [Diagnostics.ProcessStartInfo]::new()
	$info.FileName = $file; $info.WorkingDirectory = $directory
	$info.UseShellExecute = $false; $info.CreateNoWindow = $true
	$info.RedirectStandardOutput = $true; $info.RedirectStandardError = $true
	foreach ($argument in $arguments) { $info.ArgumentList.Add($argument) }
	# Never allow a caller's filter to turn a full counted regression into an
	# empty/partial test selection that appears to qualify the same controller.
	$null = $info.Environment.Remove('TERRAENGINE_TEST_FILTER')
	$null = $info.Environment.Remove('TERRAENGINE_TEST_EXCLUDE_FILTER')
	$process = [Diagnostics.Process]::new(); $process.StartInfo = $info
	try {
		if (-not $process.Start()) { throw 'Could not start sensitivity child process.' }
		$stdoutTask = $process.StandardOutput.ReadToEndAsync(); $stderrTask = $process.StandardError.ReadToEndAsync()
		$timedOut = -not $process.WaitForExit($seconds * 1000)
		if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
		$stdout = $stdoutTask.GetAwaiter().GetResult(); $stderr = $stderrTask.GetAwaiter().GetResult()
		[IO.File]::WriteAllText($logStem + '.stdout.log', $stdout)
		[IO.File]::WriteAllText($logStem + '.stderr.log', $stderr)
		return [ordered]@{ file = $file; arguments = $arguments; exitCode = $process.ExitCode; timedOut = $timedOut
			stdoutPath = $logStem + '.stdout.log'; stderrPath = $logStem + '.stderr.log' }
	} finally { $process.Dispose() }
}
function Read-TestResult($run) {
	$text = [IO.File]::ReadAllText($run.stdoutPath)
	$summary = [regex]::Matches($text, '(?m)^(\d+) selected tests \((\d+) registered\), (\d+) failures\r?$')
	if ($run.timedOut -or $summary.Count -ne 1) { throw 'Missing unique normal test completion summary; crashes/timeouts cannot establish sensitivity.' }
	$selected = [int]$summary[0].Groups[1].Value; $registered = [int]$summary[0].Groups[2].Value
	$failures = [int]$summary[0].Groups[3].Value
	if ($selected -le 0 -or $selected -ne $registered) { throw 'Sensitivity requires the complete nonempty registered suite.' }
	$caseNames = @([regex]::Matches($text, '(?m)^\[(?:PASS|FAIL|ERROR)\] ([^:\r\n]+)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object)
	if ($caseNames.Count -ne $selected) { throw 'Test case records disagree with the completion count.' }
	return [ordered]@{ selected = $selected; registered = $registered; failures = $failures; caseNames = $caseNames
		expectedOwnershipAssertionFailed = $text.Contains($expectedAssertion); hasErrors = $text.Contains('[ERROR]') }
}

try {
	foreach ($relative in $relativeFiles) {
		$receipt.sourceManifest += [ordered]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath (Join-Path $SourceRoot $relative) -Algorithm SHA256).Hash }
	}
	Check-SharedSources
	$pristine = Join-Path $WorkRoot 'pristine'; Copy-Closure $pristine
	$original = [IO.File]::ReadAllBytes((Join-Path $pristine $controllerPath))
	$encoding = [Text.UTF8Encoding]::new($false, $true)
	$text = $encoding.GetString($original)
	$anchor = 'case ReadbackFenceObservation::NotReady:'
	$anchors = [regex]::Matches($text, [regex]::Escape($anchor))
	if ($anchors.Count -ne 1) { throw 'Production NotReady branch is missing or ambiguous; mutation must be reviewed against current source.' }
	$insertionOffset = $encoding.GetByteCount($text.Substring(0, $anchors[0].Index + $anchor.Length))
	$inserted = $encoding.GetBytes(' Retire(adapter, ReadbackRetirementProof::FenceCompleted);')
	$mutated = [byte[]]::new($original.Length + $inserted.Length)
	[Buffer]::BlockCopy($original, 0, $mutated, 0, $insertionOffset)
	[Buffer]::BlockCopy($inserted, 0, $mutated, $insertionOffset, $inserted.Length)
	[Buffer]::BlockCopy($original, $insertionOffset, $mutated, $insertionOffset + $inserted.Length, $original.Length - $insertionOffset)
	$restored = [byte[]]::new($original.Length)
	[Buffer]::BlockCopy($mutated, 0, $restored, 0, $insertionOffset)
	[Buffer]::BlockCopy($mutated, $insertionOffset + $inserted.Length, $restored, $insertionOffset, $original.Length - $insertionOffset)
	if (-not [Linq.Enumerable]::SequenceEqual[byte]($original, $restored)) { throw 'Mutation is not exactly byte-reversible.' }
	# The sole insertion contains no comment delimiter or newline. Every original
	# byte survives in order, including all comments, compiler directives and tests.
	$receipt.mutation = [ordered]@{ path = $controllerPath; insertionOffset = $insertionOffset
		insertedBase64 = [Convert]::ToBase64String($inserted); byteReversible = $true; commentsAndNewlinesUnchanged = $true
		meaning = 'Intentionally authorize native retirement on NotReady using a false FenceCompleted decision.' }
	$problems = [Collections.Generic.List[string]]::new()
	foreach ($name in @('correct-before', 'mutant', 'correct-after')) {
		Check-SharedSources
		$variantRoot = Join-Path $WorkRoot $name; $sourceCopy = Join-Path $variantRoot 'src'; Copy-Closure $sourceCopy
		if ($name -eq 'mutant') { [IO.File]::WriteAllBytes((Join-Path $sourceCopy $controllerPath), $mutated) }
		$objects = Join-Path $variantRoot 'obj'; New-Item -ItemType Directory -Path $objects | Out-Null
		$exe = Join-Path $variantRoot 'ReadbackLifetimeSensitivity.exe'
		$responsePath = Join-Path $variantRoot 'compile.rsp'
		$response = @($flags)
		$response += '/I"' + (Join-Path $sourceCopy 'Engine') + '"'
		$response += '/I"' + (Join-Path $sourceCopy 'Tests/Engine') + '"'
		$response += '/Fo"' + ($objects.Replace('\', '/') + '/') + '"'
		$response += '/Fe"' + $exe + '"'
		$response += '"' + (Join-Path $sourceCopy $controllerPath) + '"'
		$response += '"' + (Join-Path $sourceCopy 'Tests/Engine/Renderer/VulkanReadbackLifetimeTests.cpp') + '"'
		[IO.File]::WriteAllLines($responsePath, $response, $encoding)
		$variant = [ordered]@{ name = $name; sourceHashes = @(); responsePath = $responsePath
			responseSha256 = (Get-FileHash -LiteralPath $responsePath -Algorithm SHA256).Hash; compile = $null; run = $null; tests = $null; executableSha256 = $null }
		foreach ($relative in $relativeFiles) { $variant.sourceHashes += [ordered]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath (Join-Path $sourceCopy $relative) -Algorithm SHA256).Hash } }
		$receipt.variants += $variant; Save-Receipt
		if ($VcVarsPath) {
			# A literal driver avoids passing embedded command quotes through .NET's
			# Windows argv escaping, which cmd does not interpret like a C runtime.
			$driverPath = Join-Path $variantRoot 'compile.cmd'
			$driver = @('@echo off', ('call "' + $VcVarsPath + '" >nul'), 'if errorlevel 1 exit /b %errorlevel%',
				('"' + $CompilerPath + '" @"' + $responsePath + '"'), 'exit /b %errorlevel%')
			[IO.File]::WriteAllLines($driverPath, $driver, $encoding)
			$variant.compilerDriverPath = $driverPath
			$variant.compilerDriverSha256 = (Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash
			$variant.compile = Run-Process (Join-Path $env:SystemRoot 'System32/cmd.exe') @('/d', '/c', $driverPath) $variantRoot (Join-Path $variantRoot 'compile') $CompileTimeoutSeconds
		} else { $variant.compile = Run-Process $CompilerPath @('@' + $responsePath) $variantRoot (Join-Path $variantRoot 'compile') $CompileTimeoutSeconds }
		if ($variant.compile.exitCode -ne 0 -or $variant.compile.timedOut -or -not (Test-Path -LiteralPath $exe)) {
			$problems.Add("$name compilation did not succeed; compiler failure is not mutation sensitivity.")
			if ($name -eq 'correct-before') { throw $problems[$problems.Count - 1] }
			continue
		}
		$variant.executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
		$variant.run = Run-Process $exe @() $variantRoot (Join-Path $variantRoot 'tests') $TestTimeoutSeconds
		try {
			$variant.tests = Read-TestResult $variant.run
			if ($name -eq 'mutant') {
				if ($variant.run.exitCode -ne 1 -or -not $variant.tests.expectedOwnershipAssertionFailed -or $variant.tests.hasErrors) { $problems.Add('Mutant did not fail through the expected counted ownership assertion.') }
			} elseif ($variant.run.exitCode -ne 0 -or $variant.tests.failures -ne 0 -or $variant.tests.hasErrors) { $problems.Add("$name did not pass the correct production regression.") }
		} catch { $problems.Add("${name}: $($_.Exception.Message)") }
		Save-Receipt
		if ($name -eq 'correct-before' -and $problems.Count) { throw $problems[$problems.Count - 1] }
	}
	if ($receipt.variants.Count -ne 3 -or $problems.Count) { throw ($problems -join ' ') }
	$expectedNames = $receipt.variants[0].tests.caseNames -join "`n"
	foreach ($variant in $receipt.variants) {
		if (($variant.tests.caseNames -join "`n") -ne $expectedNames) { throw 'Variants executed different counted regression case sets.' }
		foreach ($entry in $variant.sourceHashes) {
			$actual = (Get-FileHash -LiteralPath (Join-Path (Join-Path $WorkRoot $variant.name) ('src/' + $entry.path)) -Algorithm SHA256).Hash
			if ($actual -ne $entry.sha256) { throw 'Isolated source bytes changed after compilation.' }
		}
	}
	Check-SharedSources; $receipt.sharedSourcesUnchanged = $true; $receipt.status = 'passed'
} catch {
	$receipt.failure = $_.Exception.Message
	throw
} finally {
	try { Check-SharedSources; $receipt.sharedSourcesUnchanged = $true } catch { $receipt.sharedSourcesUnchanged = $false; $receipt.status = 'incomplete'; $receipt.failure = $_.Exception.Message }
	$receipt.completedUtc = [DateTime]::UtcNow.ToString('o'); Save-Receipt
	Write-Output "Readback sensitivity $($receipt.status): $receiptPath"
}
