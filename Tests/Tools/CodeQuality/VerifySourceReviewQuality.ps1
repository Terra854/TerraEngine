<#
Portable checks use committed manifests/current source, never
ignored before copies. PrivateProof additionally replays actual comment-only
comparisons against parent-sealed pre-cleanup copies under EvidenceRoot. Neither mode
proves semantic callable discovery, ownership safety or runtime parity.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$RepoRoot,
	[Parameter(Mandatory)][string]$AuditExecutable,
	[Parameter(Mandatory)][string]$StageValidator,
	[Parameter(Mandatory)][string]$StageManifest,
	[Parameter(Mandatory)][string]$Dispositions,
	[Parameter(Mandatory)][string]$ProductionComments,
	[Parameter(Mandatory)][string]$CommentBatches,
	[string]$OutputPath = ([IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('TerraEngine-source-review-quality-'+[guid]::NewGuid().ToString('N')+'.json')))),
	[ValidateSet('Portable','PrivateProof')][string]$Mode = 'Portable',
	[string]$EvidenceRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require([bool]$Condition, [string]$Message) { if(!$Condition) { throw $Message } }
function Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function StringValue($Value, [string]$Label) {
	Require ($Value -is [string] -and ![string]::IsNullOrWhiteSpace($Value)) "Missing string: $Label"
	Require ($Value -notmatch '^(\?|TODO|TBD|Unknown|placeholder)$') "Placeholder: $Label"
	return $Value
}
function ShaValue($Value) { $null=StringValue $Value 'sha256'; Require ($Value -cmatch '^[0-9a-f]{64}$') 'Invalid SHA256'; return $Value }
function Fields($Object, [string[]]$Names, [string]$Label) {
	Require ($Object -is [Collections.IDictionary]) "Expected object: $Label"
	foreach($name in $Names) { Require ($Object.Contains($name)) "Missing $Label.$name" }
}
function ArrayValue($Value, [string]$Label, [bool]$AllowEmpty=$false) {
	Require ($Value -is [array] -and ($AllowEmpty -or $Value.Count -gt 0)) "Missing/empty array: $Label"
}
function PositiveLine($Value) {
	Require ($Value -isnot [string] -and $Value -isnot [bool] -and [string]$Value -cmatch '^[1-9][0-9]*$') 'Invalid physical line'
	return [int]$Value
}
function UnsignedValue($Value) {
	Require ($null -ne $Value -and $Value -isnot [string] -and $Value -isnot [bool] -and [string]$Value -cmatch '^(0|[1-9][0-9]*)$') 'Invalid unsigned JSON integer'
	return [uint64]$Value
}
function NoRedirect([string]$Path) {
	$item=Get-Item -LiteralPath $Path -Force
	while($null -ne $item) {
		Require (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Redirected path: $Path"
		$item=if($item -is [IO.DirectoryInfo]) {$item.Parent} else {$item.Directory}
	}
}
function Full([string]$Path) {
	Require ([IO.Path]::IsPathRooted($Path)) "Expected absolute path: $Path"
	$full=[IO.Path]::GetFullPath($Path); NoRedirect $full; return $full
}
function Under([string]$Root,[string]$Relative) {
	$null=StringValue $Relative 'relative path'; Require (![IO.Path]::IsPathRooted($Relative)) 'Rooted relative path'
	$full=[IO.Path]::GetFullPath((Join-Path $Root $Relative)); $canonical=[IO.Path]::GetRelativePath($Root,$full).Replace('\','/')
	Require ($canonical -ceq $Relative -and !$canonical.StartsWith('../') -and $canonical -ne '..') "Escaping/noncanonical path: $Relative"
	NoRedirect $full; return $full
}
function UniqueJson($Element) {
	if($Element.ValueKind -eq [Text.Json.JsonValueKind]::Object) {
		$names=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		foreach($property in $Element.EnumerateObject()) { Require ($names.Add($property.Name)) "Duplicate JSON field: $($property.Name)"; UniqueJson $property.Value }
	} elseif($Element.ValueKind -eq [Text.Json.JsonValueKind]::Array) { foreach($item in $Element.EnumerateArray()) { UniqueJson $item } }
}
function Json([string]$Text) {
	$options=[Text.Json.JsonDocumentOptions]::new(); $options.MaxDepth=100
	$document=[Text.Json.JsonDocument]::Parse($Text,$options)
	try { UniqueJson $document.RootElement } finally { $document.Dispose() }
	return ConvertFrom-Json -InputObject $Text -AsHashtable -Depth 100
}
function ReadJson([string]$Path) { return Json ([IO.File]::ReadAllText($Path)) }
function WriteFresh([string]$Path,$Object) {
	$stream=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
	try { $bytes=[Text.Encoding]::UTF8.GetBytes(($Object | ConvertTo-Json -Depth 100)); $stream.Write($bytes,0,$bytes.Length) } finally { $stream.Dispose() }
}
function Native([string[]]$Arguments,[int]$ExpectedExit=0) {
	$lines=& $script:tool @Arguments 2>&1; $code=$LASTEXITCODE
	Require ($code -eq $ExpectedExit) "Native audit exit $code for $($Arguments[0]): $($lines -join ' ')"
	return ($lines -join "`n")
}

$result=[ordered]@{schemaVersion='source-review-quality-v1';status='Failed';mode=$Mode;
	structuralOnly=$true;semanticCompletenessProven=$false;lifetimeAcceptance=$false;tokenProofExecuted=$false}
$canWrite=$false; $exitCode=1
try {
	Require ([IO.Path]::IsPathRooted($OutputPath)) 'OutputPath must be absolute'
	$output=[IO.Path]::GetFullPath($OutputPath); Require (!(Test-Path -LiteralPath $output)) 'OutputPath must be fresh'
	$parent=[IO.Path]::GetDirectoryName($output); NoRedirect $parent; $canWrite=$true
	$root=Full $RepoRoot; $script:tool=Full $AuditExecutable
	$stageScript=Full $StageValidator; $stagePath=Full $StageManifest; $ledgerPath=Full $Dispositions
	$originalPath=Full $ProductionComments; $batchesPath=Full $CommentBatches
	$inputs=@((Full $PSCommandPath),$script:tool,$stageScript,$stagePath,$ledgerPath,$originalPath,$batchesPath)
	$pins=@{}; foreach($path in $inputs) { Require ([IO.File]::Exists($path)) "Missing file: $path"; $pins[$path]=Hash $path }
	# Frozen constants prevent a coordinated rewrite of both the baseline and ledger.
	$originalSha='8dc0b9a78a80aaf8cfd86d046c652353be6f156f1296f503a491e65e9dabbc26'
	$batchesSha='6d13e2f9a72d073b5e8101b8e23827b4714568663cb02ac3ff17e23cb875e985'
	$revision='0b9081841bd262ebacf1f2d1077875407916d296'
	Require ($pins[$originalPath] -ceq $originalSha -and $pins[$batchesPath] -ceq $batchesSha) 'Frozen inventory/batches changed'
	$original=ReadJson $originalPath; $batches=ReadJson $batchesPath; $ledger=ReadJson $ledgerPath
	Require ($original.baselineRevision -ceq $revision -and $batches.baselineRevision -ceq $revision) 'Wrong baseline revision'
	Fields $ledger @('schemaVersion','baselineRevision','productionCommentsSha256','commentBatchesSha256','beforeManifestSha256','batches','commentOnlyFiles','occurrences') 'ledger'
	$null=ShaValue $ledger.beforeManifestSha256
	Require ($ledger.schemaVersion -ceq 'source-review-comment-dispositions-v1' -and $ledger.baselineRevision -ceq $revision -and $ledger.productionCommentsSha256 -ceq $originalSha -and $ledger.commentBatchesSha256 -ceq $batchesSha) 'Wrong disposition schema/baseline pins'
	foreach($key in @('batches','commentOnlyFiles','occurrences')) { ArrayValue $ledger[$key] "ledger.$key" }
	$batchByPath=@{}; $batchCounts=@{}
	foreach($batch in $batches.batches) {
		Require (!$batchCounts.ContainsKey($batch.id)) 'Duplicate frozen batch'; $batchCounts[$batch.id]=[int]$batch.occurrenceCount
		foreach($file in $batch.files) { Require (!$batchByPath.ContainsKey($file.path)) 'Duplicate frozen file ownership'; $batchByPath[$file.path]=$batch.id }
	}
	Require ($batchCounts.Count -eq 6 -and $batchByPath.Count -eq 170) 'Wrong frozen batch/file count'
	$expected=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal); $filePins=@{}
	foreach($file in $original.files) {
		Require (!$filePins.ContainsKey($file.path) -and $batchByPath.ContainsKey($file.path)) 'Duplicate/unassigned original file'
		$filePins[$file.path]=$file.sha256; $ordinals=@{}
		foreach($occurrence in $file.occurrences) {
			$line=PositiveLine $occurrence.line; if(!$ordinals.ContainsKey($line)) {$ordinals[$line]=0}; $ordinals[$line]++
			$id="$($file.path)|$($file.sha256)|$line|$($ordinals[$line])"
			Require (!$expected.ContainsKey($id)) 'Duplicate original occurrence ID'
			$expected[$id]=@{path=$file.path;line=$line;text=$occurrence.text;sha256=$file.sha256;batch=$batchByPath[$file.path]}
		}
	}
	Require ($expected.Count -eq 2345 -and $filePins.Count -eq 170 -and $original.occurrenceCount -eq 2345) 'Wrong original occurrence closure'
	$declaredBatches=@{}
	foreach($batch in $ledger.batches) {
		Fields $batch @('id','occurrenceCount') 'ledger batch'
		Require ($batchCounts.ContainsKey($batch.id) -and !$declaredBatches.ContainsKey($batch.id) -and (UnsignedValue $batch.occurrenceCount) -eq $batchCounts[$batch.id]) 'Missing/duplicate/wrong batch count'
		$declaredBatches[$batch.id]=0
	}
	Require ($declaredBatches.Count -eq 6) 'All six batches, including empty animation, are required'
	$proofs=@{}; $proofPaths=@{}; $currentPins=@{}; $currentLines=@{}
	foreach($pair in $ledger.commentOnlyFiles) {
		Fields $pair @('id','batchId','path','beforeSha256','afterSha256','beforeRelativePath','intentOnly','intent') 'comment-only file'
		$id=StringValue $pair.id 'proof ID'; Require (!$proofs.ContainsKey($id) -and !$proofPaths.ContainsKey($pair.path)) 'Duplicate proof ID/path'
		Require ($batchCounts.ContainsKey($pair.batchId)) 'Proof file has unknown batch'
		Require ($pair.beforeRelativePath -is [string] -and $pair.beforeRelativePath -cmatch '^[^:\\]+$' -and !$pair.beforeRelativePath.StartsWith('/') -and @($pair.beforeRelativePath.Split('/') | Where-Object {$_ -in @('','.','..')}).Count -eq 0) 'Nonportable beforeRelativePath'
		$null=ShaValue $pair.beforeSha256; $null=ShaValue $pair.afterSha256; $null=StringValue $pair.intent 'retained/rewrite intent'
		Require ($pair.intentOnly -is [bool]) 'intentOnly must be a JSON boolean'
		$current=Under $root $pair.path; Require ($pair.path.StartsWith('Engine/') -or $pair.path.StartsWith('Editor/')) 'Comment-only file outside production'
		Require ((Hash $current) -ceq $pair.afterSha256) "Stale after pin: $($pair.path)"
		$proofs[$id]=$pair; $proofPaths[$pair.path]=$id; $currentPins[$current]=$pair.afterSha256; $currentLines[$id]=[IO.File]::ReadAllLines($current)
	}
	$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal); $usedProofs=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	$currentLocations=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach($row in $ledger.occurrences) {
		Fields $row @('occurrenceId','batchId','originalPath','originalSha256','originalLine','originalText','action','reason','proofPairId','currentPath','currentLine','beforeLineText','replacementLine','relocated') 'disposition'
		$id=StringValue $row.occurrenceId 'occurrence ID'; Require ($expected.ContainsKey($id) -and $seen.Add($id)) 'Unknown/duplicate original occurrence'
		$e=$expected[$id]; Require ($row.batchId -ceq $e.batch -and $row.originalPath -ceq $e.path -and $row.originalSha256 -ceq $e.sha256 -and (PositiveLine $row.originalLine) -eq $e.line -and $row.originalText -ceq $e.text) 'Original occurrence mismatch'
		Require ($row.action -cin @('remove','rewrite')) 'Unsupported disposition action'; $null=StringValue $row.reason 'reviewed reason'
		Require ($proofs.ContainsKey($row.proofPairId) -and $row.currentPath -ceq $proofs[$row.proofPairId].path) 'Missing/inconsistent proof file'
		Require ($row.batchId -ceq $proofs[$row.proofPairId].batchId) 'Relocation crossed batch ownership without reconciliation'
		$line=PositiveLine $row.currentLine; $lines=$currentLines[$row.proofPairId]; Require ($line -le $lines.Count) 'Current line out of bounds'
		Require ($currentLocations.Add("$($row.currentPath)|$line")) 'Duplicate final occurrence location'
		Require ($row.relocated -is [bool] -and $row.relocated -eq ($row.currentPath -cne $e.path -or $line -ne $e.line)) 'Unmatched relocation flag'
		$null=StringValue $row.beforeLineText 'pre-comment line text'; Require ($row.replacementLine -is [string]) 'replacementLine must be a string'
		Require (@($original.commentPatterns | Where-Object {$row.beforeLineText.Contains($_)}).Count -gt 0) 'Pre-comment line contains no frozen filler pattern'
		if($row.action -ceq 'remove') { Require ($row.replacementLine -ceq '' -and [string]::IsNullOrWhiteSpace($lines[$line-1])) 'Removed occurrence is not a blank line' }
		else { Require ($row.replacementLine -match '^\s*//' -and $lines[$line-1] -ceq $row.replacementLine) 'Rewritten line mismatch' }
		$declaredBatches[$row.batchId]++; $null=$usedProofs.Add($row.proofPairId)
	}
	Require ($seen.Count -eq 2345) 'Original occurrence omissions'
	foreach($id in $batchCounts.Keys) { Require ($declaredBatches[$id] -eq $batchCounts[$id]) 'Per-batch occurrence mismatch' }
	foreach($pair in $proofs.Values) { Require ($usedProofs.Contains($pair.id) -or $pair.intentOnly) 'Unreferenced proof file must explicitly be intent-only' }
	if($Mode -ceq 'PrivateProof') {
		# Longer comment text keeps executable spelling equal but moves the next
		# token's physical column. Leading code whitespace alone is insufficient:
		# the legacy spacing check already rejects that weaker probe.
		$probe=Join-Path $parent ('source-review-probes-'+[guid]::NewGuid().ToString('N')); $null=New-Item -ItemType Directory -Path $probe
		$left=Join-Path $probe 'left.cpp'; $right=Join-Path $probe 'right.cpp'
		[IO.File]::WriteAllText($left,"int/*a*/value = 0;`n"); [IO.File]::WriteAllText($right,"int/*longer*/value = 0;`n")
		$null=Native -Arguments @('--compare',$left,$left); $null=Native -Arguments @('--compare',$left,$right) -ExpectedExit 1
		$result.nativeByteColumnCapabilityChecked=$true
	}
	# Strict native selection must be nonempty, parse successfully and contain no patterns.
	$selfTest=Native -Arguments @('--self-test'); Require ($selfTest -match 'SourceReviewAudit self-test failures=0') 'Unsupported native self-test output'
	if($Mode -ceq 'PrivateProof') {
		$evidence=Full $EvidenceRoot
		$beforeManifest=Under $evidence 'pre-comment-manifest.json'
		Require ((Hash $beforeManifest) -ceq $ledger.beforeManifestSha256) 'Unsealed/missing pre-comment manifest'
		$pins[$beforeManifest]=$ledger.beforeManifestSha256
		$beforeRecords=ReadJson $beforeManifest; Fields $beforeRecords @('files') 'pre-comment manifest'; ArrayValue $beforeRecords.files 'before files'
		Require ($beforeRecords.files.Count -eq $proofs.Count) 'Pre-comment manifest file closure mismatch'
		$beforeIds=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		foreach($file in $beforeRecords.files) {
			Fields $file @('id','path','beforeRelativePath','beforeSha256') 'before file'
			Require ($proofs.ContainsKey($file.id) -and $beforeIds.Add($file.id)) 'Unknown/duplicate pre-comment file'
			$pair=$proofs[$file.id]
			foreach($key in @('path','beforeRelativePath','beforeSha256')) { Require ($file[$key] -ceq $pair[$key]) 'Pre-comment manifest pin mismatch' }
		}
		foreach($pair in $proofs.Values) {
			$before=Under $evidence $pair.beforeRelativePath; Require ((Hash $before) -ceq $pair.beforeSha256) 'Missing/stale immutable pre-comment pin'
			$pins[$before]=$pair.beforeSha256
			$beforeLines=[IO.File]::ReadAllLines($before)
			foreach($row in $ledger.occurrences | Where-Object proofPairId -CEQ $pair.id) {
				Require ($row.currentLine -le $beforeLines.Count -and $beforeLines[$row.currentLine-1] -ceq $row.beforeLineText) 'Relocated pre-comment line does not match sealed copy'
			}
			$comparison=Native -Arguments @('--compare',$before,(Under $root $pair.path)); Require ($comparison -ceq 'equivalent executable tokens') 'Unsupported native comparator success output'
			Require ((Hash $before) -ceq $pair.beforeSha256) 'Pre-comment bytes changed during proof'
		}
		$result.tokenProofExecuted=$true
	}
	$beforeInventory=Json (Native -Arguments @('--inventory',$root)); Fields $beforeInventory @('files','fileCount','errorCount') 'pre-inventory'
	ArrayValue $beforeInventory.files 'pre-inventory files'; Require ($beforeInventory.errorCount -eq 0 -and $beforeInventory.fileCount -eq $beforeInventory.files.Count) 'Invalid pre-inventory'
	$productionPins=@{}
	foreach($file in $beforeInventory.files) {
		$path=Full $file.path; $relative=[IO.Path]::GetRelativePath($root,$path).Replace('\','/')
		Require (($relative.StartsWith('Engine/') -or $relative.StartsWith('Editor/')) -and !$productionPins.ContainsKey($path)) 'Duplicate/outside-production native path'
		$productionPins[$path]=Hash $path
	}
	$strict=Json (Native -Arguments @('--strict-final',$root)); Fields $strict @('files','fileCount','errorCount','occurrenceCount') 'strict native output'
	ArrayValue $strict.files 'strict files'; Require ($strict.fileCount -eq $strict.files.Count -and $strict.errorCount -eq 0 -and $strict.occurrenceCount -eq 0) 'Invalid/empty/unclean strict native inventory'
	$strictPaths=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach($file in $strict.files) {
		Fields $file @('path','errors','occurrences','braceSpans') 'native file'
		Require ($file.errors.Count -eq 0 -and $file.occurrences.Count -eq 0 -and $strictPaths.Add([IO.Path]::GetFullPath($file.path))) 'Duplicate native path, unsupported syntax or remaining filler'
	}
	foreach($path in $currentPins.Keys) { Require ($strictPaths.Contains($path)) 'Comment after-file omitted by native source traversal' }
	Require ($strictPaths.Count -eq $productionPins.Count) 'Production selection changed during verification'
	foreach($path in $productionPins.Keys) { Require ($strictPaths.Contains($path) -and (Hash $path) -ceq $productionPins[$path]) 'Production source changed during strict scan' }
	$strictPath=$output+'.native.json'; WriteFresh $strictPath $strict
	$stageOutput=$output+'.stages.json'; Require (!(Test-Path -LiteralPath $stageOutput)) 'Stage output must be fresh'
	$hostExecutable=(Get-Process -Id $PID).Path
	$stageLog=& $hostExecutable -NoProfile -File $stageScript -RepoRoot $root -StageManifest $stagePath -AuditExecutable $script:tool -OutputPath $stageOutput 2>&1
	Require ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $stageOutput)) "Stage validation failed: $($stageLog -join ' ')"
	$stage=ReadJson $stageOutput; Require ($stage.status -ceq 'Passed' -and $stage.symbols -gt 0 -and $stage.nativeBraces -gt 0) 'Missing/invalid stage receipt'

	foreach($path in $pins.Keys) { Require ((Hash $path) -ceq $pins[$path]) 'Input changed during verification' }
	foreach($path in $currentPins.Keys) { Require ((Hash $path) -ceq $currentPins[$path]) 'Candidate changed during verification' }
	foreach($path in $productionPins.Keys) { Require ((Hash $path) -ceq $productionPins[$path]) 'Production source changed after strict scan' }
	$result.status='Passed'; $result.originalOccurrences=$seen.Count; $result.batchCount=6; $result.commentOnlyFiles=$proofs.Count
	$result.nativeInventory=@{path=$strictPath;sha256=Hash $strictPath}; $result.stageReceipt=@{path=$stageOutput;sha256=Hash $stageOutput}; $result.inputPins=$pins
	$result.qualification=if($Mode -ceq 'Portable') {'Portable final-source/accounting checks only; private before/after token proof was not executed.'} else {'Portable checks plus native comparisons against hash-pinned pre-comment copies; no semantic/runtime acceptance.'}
	$exitCode=0
} catch { $result.error=$_.Exception.Message }
finally {
	if($canWrite) { try { WriteFresh $output $result; Write-Output "SourceReviewQuality receipt: $output" } catch { $exitCode=1; [Console]::Error.WriteLine($_.Exception.Message) } }
	else { [Console]::Error.WriteLine($result.error) }
}
exit $exitCode
