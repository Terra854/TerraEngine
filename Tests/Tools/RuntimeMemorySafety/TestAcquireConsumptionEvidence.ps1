# Actual formatter unit-contract probe, never a GPU/semaphore acceptance lane.
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$SourceRoot,
	[Parameter(Mandatory=$true)][string]$EvidenceDirectory,
	[string]$HelperHeader,
	[string]$Cxx='cl'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$EvidenceDirectory = [IO.Path]::GetFullPath($EvidenceDirectory)
[IO.Directory]::CreateDirectory($EvidenceDirectory) | Out-Null
$caseRows = [Collections.Generic.List[object]]::new()
$childRows = [Collections.Generic.List[object]]::new()
$privateOwner = [Guid]::NewGuid().ToString('N')
$privateParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
$privateRoot = Join-Path $privateParent ("TerraEngine-ReviewScript-$privateOwner")
[IO.Directory]::CreateDirectory($privateRoot) | Out-Null
[IO.File]::WriteAllText((Join-Path $privateRoot '.review-owner'), $privateOwner)

function Assert-Contract([bool]$Condition, [string]$Message) {
	if (-not $Condition) { throw $Message }
}
function Invoke-Case([string]$Name, [scriptblock]$Body) {
	try {
		& $Body | Out-Null
		$caseRows.Add([pscustomobject]@{name=$Name;status='passed-unit-contract';reason='Actual production script/producer contract executed; no performance acceptance claimed.'})
	} catch {
		$caseRows.Add([pscustomobject]@{name=$Name;status='failed-contract';reason=$_.Exception.Message})
	}
}
function Invoke-ProductionScript([string]$Script, [string[]]$ScriptArguments = @()) {
	$pwsh = (Get-Command pwsh -ErrorAction Stop).Source
	return Invoke-BoundedChild $pwsh (@('-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',$Script) + $ScriptArguments)
}
function Invoke-BoundedChild([string]$Executable, [string[]]$ChildArguments) {
	$startInfo = [Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = $Executable
	$startInfo.UseShellExecute = $false
	$startInfo.CreateNoWindow = $true
	$startInfo.RedirectStandardOutput = $true
	$startInfo.RedirectStandardError = $true
	foreach ($argument in $ChildArguments) { $startInfo.ArgumentList.Add($argument) }
	$child = [Diagnostics.Process]::new()
	$child.StartInfo = $startInfo
	try {
		[void]$child.Start()
		$stdout = $child.StandardOutput.ReadToEndAsync()
		$stderr = $child.StandardError.ReadToEndAsync()
		$timedOut = -not $child.WaitForExit(30000)
		if ($timedOut) { $child.Kill($true); [void]$child.WaitForExit(5000) }
		$outText = if ($stdout.Wait(5000)) { $stdout.Result } else { '<stdout incomplete>' }
		$errText = if ($stderr.Wait(5000)) { $stderr.Result } else { '<stderr incomplete>' }
		$result = [pscustomobject]@{exitCode=if($timedOut){124}else{$child.ExitCode};stdout=$outText;stderr=$errText;timedOut=$timedOut}
		$childRows.Add([pscustomobject]@{executable=$Executable;arguments=$ChildArguments;result=$result})
		return $result
	} finally { $child.Dispose() }
}
function Write-UnitJson([string]$Path, $Value) {
	[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path)) | Out-Null
	[IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 24), [Text.UTF8Encoding]::new($false))
}
function Read-UnitJson([string]$Path) { Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json }
function Has-UnitProperty($Value, [string]$Name) { return $null -ne $Value -and $null -ne $Value.PSObject.Properties[$Name] }
function Complete-UnitScript([string]$Suite) {
	$failed = @($caseRows | Where-Object status -eq 'failed-contract').Count
	$unverified = @($caseRows | Where-Object status -eq 'unverified-prerequisite').Count
	$result = [ordered]@{suite=$Suite;qualification='parser/script-unit-inputs; not native GPU/performance measurement';selectedCases=$caseRows.Count;failedCases=$failed;unverifiedCases=$unverified;cases=$caseRows.ToArray();executions=$childRows.ToArray()}
	Write-UnitJson (Join-Path $EvidenceDirectory 'result.json') $result
	[Console]::Out.WriteLine(($result | ConvertTo-Json -Depth 12))
	if ($failed -gt 0) { return 1 }
	if ($unverified -gt 0) { return 2 }
	return 0
}
function Remove-PrivateRoot {
	# Verify the exact exclusively owned absolute target immediately before removal.
	$resolved = (Resolve-Path -LiteralPath $privateRoot).Path.TrimEnd([IO.Path]::DirectorySeparatorChar)
	$parent = [IO.Directory]::GetParent($resolved).FullName.TrimEnd([IO.Path]::DirectorySeparatorChar)
	if ($parent -ne $privateParent -or [IO.File]::ReadAllText((Join-Path $resolved '.review-owner')) -ne $privateOwner) {
		throw 'Refusing cleanup outside the exclusively owned temporary root.'
	}
	Remove-Item -LiteralPath $resolved -Recurse -Force
}

# The compiled CPU driver executes the exact extracted production formatter.
# Independent before/after values are unit observations, never a GPU submission.
try {
	if([string]::IsNullOrWhiteSpace($HelperHeader)){$HelperHeader=Join-Path $SourceRoot 'Tests/Tools/RuntimeMemorySafety/RuntimeMemorySafetyAcquireEvidence.h'}
	$compiler=Get-Command $Cxx -ErrorAction SilentlyContinue
	if(-not(Test-Path -LiteralPath $HelperHeader -PathType Leaf) -or $null -eq $compiler){
		$caseRows.Add([pscustomobject]@{name='AcquireEvidence.RequiredActualFormatter';status='unverified-prerequisite';reason='Approved actual producer formatter helper and a C++20 developer shell are required; no serializer model substituted.'})
		$exitCode=Complete-UnitScript 'AcquireConsumptionEvidence'
	} else {
		$HelperHeader=(Resolve-Path -LiteralPath $HelperHeader).Path
		$driver=Join-Path $EvidenceDirectory 'acquire-formatter-driver.cpp'
		$executable=Join-Path $EvidenceDirectory 'acquire-formatter-driver.exe'
		$code='#include "'+$HelperHeader.Replace('\','/')+'"'+[Environment]::NewLine+@'
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using Json=nlohmann::json;
int main(int argc,char**argv){
	if(argc!=2)return 2;
	std::ifstream stream(argv[1]);Json input;stream>>input;
	if(!input.is_array()||input.size()>8)return 2;
	Json rows=Json::array();
	for(const auto&observation:input){
		if(!observation.is_object()||!observation.contains("before")||!observation.contains("afterObservation")
			||!observation.at("before").is_boolean()||!observation.at("afterObservation").is_boolean())continue;
		TerraEngine::Renderer::Vulkan::EmptyFrameValidationReceipt value{};
		value.acquireWaitConsumedBefore=observation.at("before").get<bool>();
		const bool after=observation.at("afterObservation").get<bool>();
		const auto formatted=TerraEngine::Validation::RuntimeMemorySafety::FormatNativeAcquireReceipt(value,after);
		rows.push_back(Json{{"before",formatted.at("acquireWaitConsumedBefore")},{"after",formatted.at("acquireWaitConsumed")}});
	}
	const bool complete=!rows.empty();
	std::cout<<Json{{"status",complete?"UnitFormatted":"Unverified"},{"qualification","copied-fact formatter unit input; no native semaphore/GPU proof"},{"rows",rows},{"nativeAcceptance",false},{"performanceAcceptance",false}}.dump();
	return complete?0:2;
}
'@
		# PowerShell's literal here-string carries ordinary C++ quotes unchanged.
		$code=$code.Replace('\"','"')
		[IO.File]::WriteAllText($driver,$code)
		$compile=Invoke-BoundedChild $compiler.Source @('/nologo','/std:c++20','/EHsc','/MD','/DNDEBUG',('/I'+(Join-Path $SourceRoot 'Engine')),('/I'+(Join-Path $SourceRoot 'External/json/single_include')),$driver,('/Fo'+(Join-Path $EvidenceDirectory 'acquire-formatter-driver.obj')),('/Fe'+$executable))
		Assert-Contract ($compile.exitCode -eq 0 -and -not $compile.timedOut) "Actual formatter CPU driver did not compile: $($compile.stdout) $($compile.stderr)"
		function Invoke-Formatter([string]$Name,$Observations){
			$path=Join-Path $EvidenceDirectory ($Name+'.json')
			# Preserve an array even for one observation; this is raw unit input.
			[IO.File]::WriteAllText($path,(ConvertTo-Json -InputObject @($Observations) -Depth 6))
			$result=Invoke-BoundedChild $executable @($path)
			Assert-Contract (-not $result.timedOut) 'Formatter CPU driver exceeded watchdog.'
			$document=$result.stdout|ConvertFrom-Json
			Assert-Contract ($document.nativeAcceptance -eq $false -and $document.performanceAcceptance -eq $false) 'Unit fixture may not manufacture native/performance acceptance.'
			return [pscustomobject]@{result=$result;document=$document}
		}
		Invoke-Case 'AcquireEvidence.ConsumingObservationChangesAfterIndependently' {
			$proof=Invoke-Formatter 'consuming-unit' @(@{before=$false;afterObservation=$true})
			Assert-Contract ($proof.result.exitCode -eq 0 -and @($proof.document.rows).Count -eq 1) 'A valid copied observation must be formatted exactly once.'
			Assert-Contract ($proof.document.rows[0].before -eq $false -and $proof.document.rows[0].after -eq $true) 'Actual production formatter did not preserve an independently supplied post-operation consumed observation.'
		}
		Invoke-Case 'AcquireEvidence.AlreadyConsumedObservationStaysTrue' {
			$proof=Invoke-Formatter 'already-consumed-unit' @(@{before=$true;afterObservation=$true})
			Assert-Contract ($proof.result.exitCode -eq 0 -and $proof.document.rows[0].before -eq $true -and $proof.document.rows[0].after -eq $true) 'Actual formatter must preserve the already-consumed copied facts.'
		}
		Invoke-Case 'AcquireEvidence.NoConsumptionObservationStaysFalse' {
			$proof=Invoke-Formatter 'not-consumed-unit' @(@{before=$false;afterObservation=$false})
			Assert-Contract ($proof.result.exitCode -eq 0 -and $proof.document.rows[0].before -eq $false -and $proof.document.rows[0].after -eq $false) 'Actual formatter must preserve independent unchanged-false facts.'
		}
		Invoke-Case 'AcquireEvidence.EmptyInputCannotProduceProof' {
			$proof=Invoke-Formatter 'empty-unit' @()
			Assert-Contract ($proof.result.exitCode -eq 2 -and $proof.document.status -eq 'Unverified' -and @($proof.document.rows).Count -eq 0) 'An empty formatter run may not produce a vacuous evidence pass.'
		}
		Invoke-Case 'AcquireEvidence.AllRejectedInputCannotProduceProof' {
			$proof=Invoke-Formatter 'all-rejected-unit' @(@{before='wrong-type';afterObservation=$true},@{before=$false})
			Assert-Contract ($proof.result.exitCode -eq 2 -and $proof.document.status -eq 'Unverified' -and @($proof.document.rows).Count -eq 0) 'All rejected raw rows may not produce a vacuous evidence pass.'
		}
		$exitCode=Complete-UnitScript 'AcquireConsumptionEvidence'
	}
} finally { Remove-PrivateRoot }
exit $exitCode
