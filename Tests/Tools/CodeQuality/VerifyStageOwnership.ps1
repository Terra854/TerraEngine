<#
Structural validation only: exact native lexical spans, source hashes, complete
stage-file/brace accounting and reviewed ownership references. This cannot prove
semantic function discovery, borrower lifetime, scheduler behavior or replay.
Requires PowerShell 7 and the bundled, built SourceReviewAudit executable.

pwsh -NoProfile -File VerifyStageOwnership.ps1 -RepoRoot <absolute-checkout> `
	-StageManifest <absolute-stage-ownership.json> -AuditExecutable <absolute-exe> `
	-OutputPath <absolute-fresh-result.json>

Only candidate is validated; frozen top-level baseline facts are never rewritten.
Every candidate ownership contract has id, owner, lastConsumer and settlement;
the latter three accept nonempty strings or arrays of nonempty strings. They are
reviewed descriptions, not machine-verifiable lifetime proofs. Every native brace
must be a named callable or an explicitly classified non-callable lexical span.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$RepoRoot,
	[Parameter(Mandatory)][string]$StageManifest,
	[Parameter(Mandatory)][string]$AuditExecutable,
	[Parameter(Mandatory)][string]$OutputPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require([bool]$Condition, [string]$Message) {
	if(!$Condition) { throw $Message }
}
function FullPath([string]$Path) {
	Require ([IO.Path]::IsPathRooted($Path)) "Expected absolute path: $Path"
	return [IO.Path]::GetFullPath($Path)
}
function NoRedirect([string]$Path) {
	$item = Get-Item -LiteralPath $Path -Force
	while($null -ne $item) {
		Require (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Redirected path is unsupported: $Path"
		$item = if($item -is [IO.DirectoryInfo]) { $item.Parent } else { $item.Directory }
	}
}
function Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function StringValue($Value, [string]$Label) {
	Require ($Value -is [string] -and ![string]::IsNullOrWhiteSpace($Value)) "Missing nonempty string: $Label"
	return $Value
}
function Fields($Object, [string[]]$Names, [string]$Label) {
	Require ($Object -is [Collections.IDictionary]) "Expected object: $Label"
	foreach($name in $Names) { Require ($Object.Contains($name)) "Missing $Label.$name" }
}
function ArrayValue($Value, [string]$Label, [bool]$AllowEmpty = $false) {
	Require ($Value -is [array]) "Expected array: $Label"
	Require ($AllowEmpty -or $Value.Count -gt 0) "Empty array: $Label"
}
function Integer($Value, [string]$Label) {
	Require ($null -ne $Value -and $Value -isnot [string] -and $Value -isnot [bool] -and
		[string]$Value -cmatch '^(0|[1-9][0-9]*)$') "Expected unsigned JSON integer: $Label"
	return [uint64]$Value
}
function Description($Value, [string]$Label) {
	$items = @(if($Value -is [array]) { $Value } else { $Value })
	Require ($items.Count -gt 0) "Empty description: $Label"
	foreach($item in $items) {
		$text = StringValue $item $Label
		Require ($text -notmatch '^(\?|Unknown|TODO|TBD|N/A|placeholder)$') "Placeholder description: $Label"
	}
}
function UniqueJson($Element) {
	# ConvertFrom-Json can otherwise discard duplicate fields before validation.
	if($Element.ValueKind -eq [Text.Json.JsonValueKind]::Object) {
		$names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		foreach($property in $Element.EnumerateObject()) {
			Require ($names.Add($property.Name)) "Duplicate JSON property: $($property.Name)"
			UniqueJson $property.Value
		}
	} elseif($Element.ValueKind -eq [Text.Json.JsonValueKind]::Array) {
		foreach($item in $Element.EnumerateArray()) { UniqueJson $item }
	}
}
function JsonObject([string]$Text) {
	$options = [Text.Json.JsonDocumentOptions]::new(); $options.MaxDepth = 100
	$document = [Text.Json.JsonDocument]::Parse($Text, $options)
	try { UniqueJson $document.RootElement } finally { $document.Dispose() }
	return ConvertFrom-Json -InputObject $Text -AsHashtable -Depth 100
}
function SourcePath([string]$Relative) {
	$null = StringValue $Relative 'source path'
	Require (![IO.Path]::IsPathRooted($Relative)) "Source path must be repository relative: $Relative"
	$full = [IO.Path]::GetFullPath((Join-Path $script:root $Relative))
	$canonical = [IO.Path]::GetRelativePath($script:root, $full).Replace('\','/')
	Require ($canonical -ceq $Relative.Replace('\','/') -and !$canonical.StartsWith('../') -and $canonical -ne '..') "Noncanonical/escaping source path: $Relative"
	Require ([IO.File]::Exists($full)) "Source file absent: $Relative"
	NoRedirect $full
	return $full
}

$result = [ordered]@{schemaVersion='stage-ownership-structural-verification-v1'; status='Failed'; structuralOnly=$true;
	semanticCompletenessProven=$false; lifetimeAcceptance=$false; files=@(); symbols=0; nonCallableBraces=0; nativeBraces=0}
$canWrite = $false
$exitCode = 1
try {
	$output = FullPath $OutputPath
	Require (!(Test-Path -LiteralPath $output)) 'OutputPath must be fresh.'
	$parent = [IO.Path]::GetDirectoryName($output)
	Require ([IO.Directory]::Exists($parent)) 'Output parent directory must already exist.'
	NoRedirect $parent; $canWrite = $true
	$script:root = FullPath $RepoRoot; NoRedirect $script:root
	$manifestPath = FullPath $StageManifest; NoRedirect $manifestPath
	$tool = FullPath $AuditExecutable; NoRedirect $tool
	$manifestHash = Hash $manifestPath; $toolHash = Hash $tool
	$result.manifest = @{path=$manifestPath;sha256=$manifestHash}
	$result.auditExecutable = @{path=$tool;sha256=$toolHash}
	$manifest = JsonObject ([IO.File]::ReadAllText($manifestPath))
	Fields $manifest @('candidate') 'manifest'
	$candidate = $manifest.candidate
	Fields $candidate @('sources','symbols','nonCallableBraces','ownershipContracts') 'candidate'
	foreach($name in @('sources','symbols','ownershipContracts')) { ArrayValue $candidate[$name] "candidate.$name" }
	ArrayValue $candidate.nonCallableBraces 'candidate.nonCallableBraces' $true
	$expected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach($domain in @('Renderer','Animation')) {
		$directory = Join-Path $script:root "Engine/$domain"
		Require ([IO.Directory]::Exists($directory)) "Stage domain absent: $domain"
		NoRedirect $directory
		$stage = Join-Path $directory 'Frame'
		Require ([IO.Directory]::Exists($stage)) "Frame directory absent: $domain"
		$entries = @(Get-ChildItem -LiteralPath $directory -File | Where-Object Name -Like ($domain.Replace('Renderer','Render')+'System*'))
		$entries += @(Get-ChildItem -LiteralPath $stage -Recurse -File)
		foreach($entry in $entries) {
			if($entry.Extension -notin @('.cpp','.h','.hpp','.inl','.hxx','.cxx','.cc')) { continue }
			NoRedirect $entry.FullName
			$null = $expected.Add([IO.Path]::GetRelativePath($script:root,$entry.FullName).Replace('\','/'))
		}
	}
	Require ($expected.Count -gt 0) 'Source closure is empty.'
	$sources = @{}; $braces = @{}; $claimed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach($source in $candidate.sources) {
		Fields $source @('path','sourceSha256') 'source'
		$path = StringValue $source.path 'source.path'; $full = SourcePath $path
		Require (!$sources.ContainsKey($path) -and $expected.Contains($path)) "Duplicate/out-of-closure source: $path"
		$sha = StringValue $source.sourceSha256 'source.sourceSha256'
		Require ($sha -cmatch '^[0-9a-f]{64}$' -and (Hash $full) -ceq $sha) "Stale source hash: $path"
		# Exactly one inventory process per listed file; no engine/runtime is loaded.
		$nativeOutput = & $tool --inventory $full 2>&1
		Require ($LASTEXITCODE -eq 0) "Native inventory failed for $path : $($nativeOutput -join ' ')"
		$native = JsonObject ($nativeOutput -join "`n")
		Fields $native @('files','fileCount','errorCount') 'native inventory'
		Require ($native.fileCount -eq 1 -and $native.errorCount -eq 0 -and $native.files.Count -eq 1) "Invalid native inventory size/errors: $path"
		$n = $native.files[0]
		Fields $n @('path','codeLines','errors','braceSpans') 'native file'
		Require ([IO.Path]::GetFullPath($n.path) -ieq $full -and $n.errors.Count -eq 0) "Native inventory file mismatch: $path"
		if($source.Contains('maxCodeLines')) {
			Require ($path -in @('Engine/Renderer/RenderSystem.cpp','Engine/Animation/AnimationSystem.cpp') -and (Integer $source.maxCodeLines 'source.maxCodeLines') -eq 2000) 'Only residual coordinators have the2000-line file budget.'
			Require ($n.codeLines -le $source.maxCodeLines) "Residual file budget exceeded: $path"
		}
		$sources[$path] = @{full=$full;sha256=$sha;native=$n}
		foreach($brace in $n.braceSpans) {
			$key = "$path`:$($brace.openingBraceOffset)"
			Require (!$braces.ContainsKey($key)) "Duplicate native brace: $key"
			$braces[$key] = $brace
		}
		# Even a listed fragment could hide statements inside a small caller's body.
		# Stage algorithms must remain separately compiled definitions, not includes.
		foreach($match in [regex]::Matches([IO.File]::ReadAllText($full),'(?im)^\s*#\s*include\s*[<"]([^>"\r\n]+\.(?:cpp|cxx|cc|inl|inc|ipp|tpp))[>"]')) {
			throw "Included implementation fragment is forbidden: $path -> $($match.Groups[1].Value)"
		}
		Require ((Hash $full) -ceq $sha) "Source changed during inventory: $path"
		$result.files += @{path=$path;sourceSha256=$sha;codeLines=$n.codeLines;braceCount=$n.braceSpans.Count}
	}
	Require ($sources.Count -eq $expected.Count) 'Manifest omits stage source/header files.'
	foreach($path in $expected) { Require ($sources.ContainsKey($path)) "Unlisted stage source: $path" }
	foreach($path in @('Engine/Renderer/RenderSystem.cpp','Engine/Animation/AnimationSystem.cpp')) {
		$row = @($candidate.sources | Where-Object path -CEQ $path)
		Require ($row.Count -eq 1 -and $row[0].Contains('maxCodeLines') -and $row[0].maxCodeLines -eq 2000) "Residual budget missing: $path"
	}
	$contracts = @{}; $used = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach($contract in $candidate.ownershipContracts) {
		Fields $contract @('id','owner','lastConsumer','settlement') 'ownershipContract'
		$id = StringValue $contract.id 'contract.id'
		Require (!$contracts.ContainsKey($id)) "Duplicate ownership contract: $id"
		foreach($field in @('owner','lastConsumer','settlement')) { Description $contract[$field] "contract.$id.$field" }
		$contracts[$id] = $contract
	}
	$symbolIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	$orchestrators = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach($symbol in $candidate.symbols) {
		Fields $symbol @('path','qualifiedSymbol','sourceSha256','signatureLine','openingBraceOffset','closingBraceOffset','spanEndOffsetExclusive','firstLine','lastLine','codeLines','maxCodeLines','symbolId','ownershipContractIds') 'symbol'
		$name = StringValue $symbol.qualifiedSymbol 'symbol.qualifiedSymbol'
		Require (!$name.Contains('?') -and $name -cnotmatch '(^|::)(TODO|TBD|Unknown|placeholder)($|::)') "Unresolved symbol: $name"
		Require ($symbolIds.Add((StringValue $symbol.symbolId 'symbol.symbolId'))) 'Duplicate symbol identity.'
		Require ($sources.ContainsKey($symbol.path) -and $symbol.sourceSha256 -ceq $sources[$symbol.path].sha256) "Stale/unlisted symbol source: $name"
		foreach($field in @('signatureLine','openingBraceOffset','closingBraceOffset','spanEndOffsetExclusive','firstLine','lastLine','codeLines','maxCodeLines')) { $null = Integer $symbol[$field] "symbol.$field" }
		$key = "$($symbol.path):$($symbol.openingBraceOffset)"
		Require ($braces.ContainsKey($key) -and $claimed.Add($key)) "Missing or multiply classified symbol brace: $name"
		$b = $braces[$key]
		foreach($field in @('spanEndOffsetExclusive','firstLine','lastLine','codeLines')) { Require ($symbol[$field] -eq $b[$field]) "Native $field mismatch: $name" }
		Require ($symbol.closingBraceOffset -eq $b.spanEndOffsetExclusive-1 -and $symbol.signatureLine -ge 1 -and $symbol.signatureLine -le $b.firstLine) "Invalid signature/closing bounds: $name"
		$orchestrator = $name -ceq 'RenderSystem::BuildFrameExecution' -or $name -ceq 'TerraEngine::Renderer::RenderSystem::BuildFrameExecution' -or $name -ceq 'TerraEngine::Animation::AnimationSystem::DeclareFrameWorkloads'
		if($orchestrator) {
			$domain = if($name.EndsWith('::BuildFrameExecution')) {'Renderer'} else {'Animation'}
			Require ($symbol.path -ceq "Engine/$domain/$($domain.Replace('Renderer','Render'))System.cpp" -and $orchestrators.Add($domain)) "Duplicate/misplaced orchestrator: $name"
		}
		$limit = if($orchestrator) {500} else {400}
		Require ($symbol.maxCodeLines -eq $limit -and $symbol.codeLines -le $limit) "Wrong/exceeded callable budget: $name"
		ArrayValue $symbol.ownershipContractIds "ownership refs for $name"
		$refs = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
		foreach($reference in $symbol.ownershipContractIds) {
			$id = StringValue $reference 'ownership reference'
			Require ($contracts.ContainsKey($id) -and $refs.Add($id)) "Unknown/duplicate ownership reference: $name -> $id"
			$null = $used.Add($id)
		}
	}
	Require ($orchestrators.Count -eq 2) 'Both named residual orchestrators must be mapped.'
	foreach($brace in $candidate.nonCallableBraces) {
		Fields $brace @('path','openingBraceOffset','spanEndOffsetExclusive','classification') 'nonCallableBrace'
		$null = Integer $brace.openingBraceOffset 'nonCallableBrace.openingBraceOffset'; $null = Integer $brace.spanEndOffsetExclusive 'nonCallableBrace.spanEndOffsetExclusive'
		Description $brace.classification 'nonCallableBrace.classification'
		$key = "$($brace.path):$($brace.openingBraceOffset)"
		Require ($braces.ContainsKey($key) -and $claimed.Add($key) -and $brace.spanEndOffsetExclusive -eq $braces[$key].spanEndOffsetExclusive) "Unknown/duplicate/inexact non-callable brace: $key"
	}
	Require ($claimed.Count -eq $braces.Count) 'Native braces are omitted from candidate classification.'
	Require ($used.Count -eq $contracts.Count) 'Unused ownership contracts are not qualified references.'
	foreach($source in $sources.Values) { Require ((Hash $source.full) -ceq $source.sha256) "Source changed during validation: $($source.full)" }
	Require ((Hash $manifestPath) -ceq $manifestHash -and (Hash $tool) -ceq $toolHash) 'Manifest/audit executable changed during validation.'
	$result.symbols=$candidate.symbols.Count; $result.nonCallableBraces=$candidate.nonCallableBraces.Count; $result.nativeBraces=$braces.Count
	$result.status='Passed'; $exitCode=0
} catch {
	$result.error=$_.Exception.Message
} finally {
	if($canWrite) {
		# The initial freshness check is only diagnostic. CreateNew is the actual
		# atomic claim, so a competing evidence writer can never be overwritten.
		try {
			$stream = [IO.File]::Open($output,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
			try {
				$bytes = [Text.Encoding]::UTF8.GetBytes(($result | ConvertTo-Json -Depth 20))
				$stream.Write($bytes,0,$bytes.Length)
			} finally { $stream.Dispose() }
		} catch {
			$exitCode = 1
			[Console]::Error.WriteLine("Could not exclusively create verification receipt: " + $_.Exception.Message)
		}
	}
}
if(!$canWrite) { [Console]::Error.WriteLine($result.error) }
exit $exitCode
