# CPU-only installed collector contracts; never launches the runner or editor.
# Prerequisite: installed runner, Windows junction support, and a fresh work root.
# Example: pwsh -File Test-RendererEfficiencyEvidenceCollection.ps1 -InstallRoot C:/Build -WorkRoot C:/Temp/evidence-test
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$InstallRoot,[Parameter(Mandatory=$true)][string]$WorkRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force
$utf8=[Text.UTF8Encoding]::new($false)
# Extract only the writer from current production; never execute the runner.
$install=[IO.Path]::GetFullPath($InstallRoot).TrimEnd('\','/')
$base=[IO.Path]::GetFullPath($WorkRoot).TrimEnd('\','/')
# Both ancestor directions are forbidden: the sandbox must not contain an install
# or live inside one. Reject reuse before any test creates filesystem content.
foreach($pair in @(@($install,$base),@($base,$install))) {
	if($pair[1].Equals($pair[0],[StringComparison]::OrdinalIgnoreCase) -or $pair[1].StartsWith($pair[0]+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'WorkRoot overlaps InstallRoot'}
}
if([IO.File]::Exists($base) -or [IO.Directory]::Exists($base)){throw 'WorkRoot must be fresh'}
$runner=Join-Path $install 'Tests/Tools/RendererEfficiency/Invoke-RendererEfficiencyValidation.ps1'
if(![IO.File]::Exists($runner)){throw 'Installed runner is missing'}
$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($runner,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Runner parser failure'}
foreach($name in @('JsonFragment','WriteEvidenceJson','Below','NoRedirect','SaveEvidence')) {
	$node=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$false))
	if($node.Count -ne 1){throw "Missing exact production helper $name"}
	Invoke-Expression $node[0].Extent.Text
}
function CanonicalSmall($value){return ConvertTo-Json -InputObject $value -Compress -Depth 100}
# Inspect both roots after extracting the exact installed redirection policy.
NoRedirect $runner;NoRedirect $base
[IO.Directory]::CreateDirectory($base)|Out-Null
# Parse an actual JSON object, matching the collector's input object/type path.
$inputJson='{"schemaVersion":1,"identity":{"runId":"small","largeInteger":9007199254740991,"floating":0.00784313725490196},"requestedOptions":{"enabled":true},"measurementPolicy":{"warmupFrames":60,"measuredFrames":300},"frames":[{"nested":{"escaped":"quote\" slash\\ newline\n Ω","n":null},"artifacts":[{"path":"frame.bin","sha256":"a"}]}],"successCriteria":{"Coverage":{"status":"Unverified","value":null}},"artifacts":[{"path":"root.bin","sha256":"b"}],"unknownRoot":{"array":[[],[null],[1]],"false":false},"unknownArray":[[],[null],[true,0,"x"]],"empty":[],"singleton":[{"a":1}],"nullRoot":null}'
$document=$inputJson|ConvertFrom-Json
$document|Add-Member -NotePropertyName scalarTypes -NotePropertyValue ([pscustomobject]@{date=[datetime]'2026-10-08T12:34:56Z';decimal=[decimal]'0.125';signed=[long]-123;unsigned=[uint64]9007199254740991})
$out=Join-Path $base 'evidence.json';WriteEvidenceJson $out $document
$parsed=[IO.File]::ReadAllText($out)|ConvertFrom-Json
$originalCanonical=CanonicalSmall $document;$firstEqual=(CanonicalSmall $parsed) -ceq $originalCanonical
# Append lifecycle to the SAME retained graph, without reloading the archive.
$document.artifacts+=@([pscustomobject]@{path='runner.lifecycle.json';sha256='c';kind='Counters';status='Valid';reason=''})
WriteEvidenceJson $out $document
$final=[IO.File]::ReadAllText($out)|ConvertFrom-Json
$appendEqual=(CanonicalSmall $final) -ceq (CanonicalSmall $document)
$before=[IO.File]::ReadAllBytes($out);$attrs=[IO.File]::GetAttributes($out);$failedClosed=$false
try{[IO.File]::SetAttributes($out,($attrs -bor [IO.FileAttributes]::ReadOnly));try{WriteEvidenceJson $out $document}catch{$failedClosed=$true}}finally{[IO.File]::SetAttributes($out,$attrs)}
$priorPreserved=[Convert]::ToBase64String($before) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($out))
$completedTemps=@([IO.Directory]::GetFiles($base,'.evidence-write-*.tmp'))
$completedTempPreserved=$completedTemps.Count -eq 1 -and
	(CanonicalSmall ([IO.File]::ReadAllText($completedTemps[0])|ConvertFrom-Json)) -ceq (CanonicalSmall $document)
# A native thread releases a real Windows reader without needing a PowerShell
# runspace. Each fixture owns its stream/thread and joins it before returning.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Threading;
public sealed class EvidenceCollectionReader : IDisposable {
	private FileStream stream;
	private readonly Thread releaseThread;
	private readonly object gate = new object();
	public EvidenceCollectionReader(string path, bool shareDelete, int releaseMilliseconds) {
		stream = new FileStream(path, FileMode.Open, FileAccess.Read,
			shareDelete ? FileShare.Read | FileShare.Delete : FileShare.Read);
		if(releaseMilliseconds >= 0) {
			releaseThread = new Thread(() => { Thread.Sleep(releaseMilliseconds); CloseStream(); });
			releaseThread.IsBackground = true;
			releaseThread.Start();
		}
	}
	private void CloseStream() { lock(gate) { if(stream != null) { stream.Dispose(); stream = null; } } }
	public string ReadText() {
		lock(gate) {
			using(var reader = new StreamReader(stream, System.Text.Encoding.UTF8, true, 1024, true)) return reader.ReadToEnd();
		}
	}
	public void Dispose() {
		CloseStream();
		if(releaseThread != null && !releaseThread.Join(2000)) throw new TimeoutException("Owned reader did not settle");
	}
}
'@
$readerCases=Join-Path $base 'readers';[IO.Directory]::CreateDirectory($readerCases)|Out-Null
$replacement=[pscustomobject]@{value='new';artifacts=@();frames=@()}
$delayedPath=Join-Path $readerCases 'delayed.json';[IO.File]::WriteAllText($delayedPath,'{"value":"old"}',$utf8)
$delayedReader=[EvidenceCollectionReader]::new($delayedPath,$false,150)
$delayedSucceeded=$false
try {try {WriteEvidenceJson $delayedPath $replacement;$delayedSucceeded=$true}catch {}}
finally {$delayedReader.Dispose()}
$delayedExact=(CanonicalSmall ([IO.File]::ReadAllText($delayedPath)|ConvertFrom-Json)) -ceq (CanonicalSmall $replacement)
$deletePath=Join-Path $readerCases 'delete-aware.json';[IO.File]::WriteAllText($deletePath,'{"value":"old"}',$utf8)
$deleteReader=[EvidenceCollectionReader]::new($deletePath,$true,-1)
$deleteSucceeded=$false;$readerStillOwnsOld=$false
try {try {WriteEvidenceJson $deletePath $replacement;$deleteSucceeded=$true;$readerStillOwnsOld=$deleteReader.ReadText() -ceq '{"value":"old"}' }catch {}}
finally {$deleteReader.Dispose()}
$deleteExact=(CanonicalSmall ([IO.File]::ReadAllText($deletePath)|ConvertFrom-Json)) -ceq (CanonicalSmall $replacement)
# A permanently held reader must produce a bounded failure, leave the prior
# bytes unchanged, and retain only this call's completely serialized candidate.
$heldDir=Join-Path $base 'held';[IO.Directory]::CreateDirectory($heldDir)|Out-Null
$heldPath=Join-Path $heldDir 'held.json';[IO.File]::WriteAllText($heldPath,'{"value":"old"}',$utf8)
$heldBefore=[IO.File]::ReadAllBytes($heldPath);$heldReader=[EvidenceCollectionReader]::new($heldPath,$false,-1)
$heldFailed=$false;$heldClock=[Diagnostics.Stopwatch]::StartNew()
try {try {WriteEvidenceJson $heldPath $replacement}catch {$heldFailed=$true}}
finally {$heldClock.Stop();$heldReader.Dispose()}
$heldUnchanged=[Convert]::ToBase64String($heldBefore) -ceq [Convert]::ToBase64String([IO.File]::ReadAllBytes($heldPath))
$heldTemps=@([IO.Directory]::GetFiles($heldDir,'.evidence-write-*.tmp'))
$heldComplete=$heldTemps.Count -eq 1 -and
	(CanonicalSmall ([IO.File]::ReadAllText($heldTemps[0])|ConvertFrom-Json)) -ceq (CanonicalSmall $replacement)
# SaveEvidence preserves native paths and every root/frame descriptor on a small
# real copied closure. No native archive or editor is accessed by this fixture.
$project=Join-Path $base 'project';$archive=Join-Path $base 'session'
[IO.Directory]::CreateDirectory((Join-Path $project 'native'))|Out-Null
[IO.Directory]::CreateDirectory($archive)|Out-Null
$payload=Join-Path $project 'native/pixel.bin';[IO.File]::WriteAllBytes($payload,[byte[]]@(1,2,3,4))
$descriptor=[pscustomobject]@{kind='Image';status='Valid';path='native/pixel.bin';sha256=(Get-FileHash $payload).Hash.ToLowerInvariant();reason=''}
$closure=[pscustomobject]@{artifacts=@($descriptor);frames=@([pscustomobject]@{artifacts=@($descriptor)});unknown='kept'}
$beforeClosure=CanonicalSmall $closure
$saved=SaveEvidence $closure $project $archive
$pathPreserved=(CanonicalSmall $closure) -ceq $beforeClosure
$copiedHash=(Get-FileHash (Join-Path $archive 'artifacts/native/pixel.bin')).Hash.ToLowerInvariant() -ceq $descriptor.sha256
$archiveCorrect=$saved -ceq (Join-Path $archive 'artifacts/evidence.json') -and [IO.File]::Exists((Join-Path $archive 'artifacts/native/pixel.bin'))
# Exercise a real junction, including a missing leaf below it. Remove only the
# contained link non-recursively; its target and bytes must remain untouched.
$target=Join-Path $base 'junction-target';$link=Join-Path $base 'junction-link'
[IO.Directory]::CreateDirectory($target)|Out-Null
$sentinel=Join-Path $target 'sentinel.bin';[IO.File]::WriteAllBytes($sentinel,[byte[]]@(9))
$junctionRejected=$false;$junctionRemoved=$false
try {
	& $env:ComSpec /d /c mklink /J $link $target | Out-Null
	if($LASTEXITCODE -ne 0){throw 'Real junction setup failed'}
	try{WriteEvidenceJson (Join-Path $link 'missing.json') $document}catch{$junctionRejected=$true}
} finally {
	if([IO.Directory]::Exists($link)){[IO.Directory]::Delete($link,$false)}
	$junctionRemoved=![IO.Directory]::Exists($link) -and [IO.File]::Exists($sentinel)
}
# Repeated descriptors must agree on exact content hash; a conflict cannot
# publish an archive document, even if the first verified bytes were copied.
$conflictArchive=Join-Path $base 'conflict';[IO.Directory]::CreateDirectory($conflictArchive)|Out-Null
$bad=[pscustomobject]@{kind='Image';status='Valid';path=$descriptor.path;sha256=('0'*64);reason=''}
$conflict=[pscustomobject]@{artifacts=@($descriptor,$bad);frames=@()}
$conflictRejected=$false
try{SaveEvidence $conflict $project $conflictArchive | Out-Null}catch{$conflictRejected=$_.Exception.Message -ceq 'Conflicting content-addressed artifact references'}
$conflictUnpublished=![IO.File]::Exists((Join-Path $conflictArchive 'artifacts/evidence.json'))
$invalidRootRejected=$false;$invalidRootPath=Join-Path $base 'invalid-root.json'
try{WriteEvidenceJson $invalidRootPath @($document)}catch{$invalidRootRejected=$true}
$cases=@(
	[ordered]@{name='JsonValuesAndUnknownFieldsRoundtrip';passed=$firstEqual},
	[ordered]@{name='RetainedLifecycleOwner';passed=$appendEqual},
	[ordered]@{name='AtomicFailurePreservesBytesAndCompletedTemporary';passed=($failedClosed -and $priorPreserved -and $completedTempPreserved)},
	[ordered]@{name='DelayedSharingReaderReleasePublishesExactDocument';passed=($delayedSucceeded -and $delayedExact)},
	[ordered]@{name='DeleteAwareReaderRetainsOldHandleAndAllowsPublish';passed=($deleteSucceeded -and $readerStillOwnsOld -and $deleteExact)},
	[ordered]@{name='PermanentSharingReaderFailsBoundedlyAndPreservesCandidate';passed=($heldFailed -and $heldUnchanged -and $heldComplete -and $heldClock.ElapsedMilliseconds -lt 2500)},
	[ordered]@{name='NativeDescriptorClosureUnchanged';passed=($pathPreserved -and $archiveCorrect -and $copiedHash)},
	[ordered]@{name='RealJunctionWriteRejected';passed=($junctionRejected -and $junctionRemoved)},
	[ordered]@{name='ConflictingDuplicateDescriptorRejected';passed=($conflictRejected -and $conflictUnpublished)},
	[ordered]@{name='NonObjectRootRejectedBeforeWrite';passed=($invalidRootRejected -and ![IO.File]::Exists($invalidRootPath))}
)
$failed=@($cases|Where-Object {!$_.passed}).Count
$report=[ordered]@{schemaVersion=1;status=$(if($failed){'Failed'}else{'Passed'});caseCount=$cases.Count;passedCount=($cases.Count-$failed);failedCount=$failed;powerShellVersion=$PSVersionTable.PSVersion.ToString();runnerSha256=(Get-FileHash $runner).Hash.ToLowerInvariant();cases=$cases;scope='CPU-only small installed-helper contract; no GPU or full archive claim'}
[IO.File]::WriteAllText((Join-Path $base 'receipt.json'),($report|ConvertTo-Json -Depth 20),$utf8)
$report|ConvertTo-Json -Depth 20
if($failed){exit 1}
Write-Output '10 evidence collection unit cases passed; synthetic CPU bytes only'
exit 0
