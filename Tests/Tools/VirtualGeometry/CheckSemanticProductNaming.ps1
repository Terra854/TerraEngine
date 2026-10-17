[CmdletBinding()]
param(
	[string]$Configuration = 'Tests/Tools/VirtualGeometry/semantic-product-naming.json',
	[string]$ReportPath = '',
	[switch]$NoFail
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# This audit keeps planning metadata available under excluded planning roots while
# preventing those identifiers from becoming product-facing filenames, source
# text, runtime strings, configuration, evidence paths, or installed guidance.
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))

function Resolve-RepositoryPath([string]$Path)
{
	if([IO.Path]::IsPathRooted($Path))
	{
		return [IO.Path]::GetFullPath($Path)
	}
	return [IO.Path]::GetFullPath((Join-Path $repositoryRoot $Path))
}

function Get-RepositoryRelativePath([string]$Path)
{
	# Windows PowerShell 5.1 lacks Path.GetRelativePath, so URI-relative paths
	# keep the installed audit compatible with both supported PowerShell hosts.
	$rootUri = [Uri]($repositoryRoot.TrimEnd('\') + '\')
	$pathUri = [Uri]$Path
	return [Uri]::UnescapeDataString($rootUri.MakeRelativeUri($pathUri).ToString()).
		Replace('\', '/')
}

function Test-ExcludedPath([string]$RelativePath, $ConfigurationDocument)
{
	if($RelativePath -in @($ConfigurationDocument.excludedFiles))
	{
		return $true
	}
	foreach($excludedRoot in @($ConfigurationDocument.excludedRoots))
	{
		if($RelativePath -eq $excludedRoot -or
			$RelativePath.StartsWith("$excludedRoot/", [StringComparison]::OrdinalIgnoreCase))
		{
			return $true
		}
	}
	return $false
}

# Inspect quote boundaries in the supplied text. This rejects a quoted token
# nested inside a longer diagnostic string, including escaped JSON quotes.
# No decoding or rewriting occurs here: the caller supplies the audited text.
function Get-CompleteQuotedLiteralStarts([string]$Text, $Literals)
{
	$starts = [Collections.Generic.HashSet[int]]::new()
	$opening = -1
	$enclosing = [char]0
	$lineComment = $false
	$blockComment = $false
	for($i = 0; $i -lt $Text.Length; ++$i)
	{
		$character = $Text[$i]
		if($lineComment)
		{
			if($character -eq [char]10) { $lineComment = $false }
			continue
		}
		if($blockComment)
		{
			if($character -eq [char]42 -and $i + 1 -lt $Text.Length -and $Text[$i + 1] -eq [char]47) { $blockComment = $false; ++$i }
			continue
		}
		if($enclosing -ne [char]0)
		{
			# JSON/C++ backslash and PowerShell backtick escapes cannot open a
			# new literal. Doubled single quotes stay inside a PS literal too.
			if($character -eq [char]92 -or $character -eq [char]96) { ++$i; continue }
			if($character -eq $enclosing)
			{
				if($i + 1 -lt $Text.Length -and $Text[$i + 1] -eq $enclosing) { ++$i; continue }
				if($Text.Substring($opening + 1, $i - $opening - 1) -cin @($Literals)) { [void]$starts.Add($opening + 1) }
				$enclosing = [char]0
			}
		}
		elseif($character -eq [char]34 -or $character -eq [char]39) { $enclosing = $character; $opening = $i }
		elseif($character -eq [char]35) { $lineComment = $true }
		elseif($character -eq [char]47 -and $i + 1 -lt $Text.Length)
		{
			if($Text[$i + 1] -eq [char]47) { $lineComment = $true; ++$i }
			elseif($Text[$i + 1] -eq [char]42) { $blockComment = $true; ++$i }
		}
	}
	# Keep the typed set as one result even when it is empty. Scan once per
	# eligible text, rather than rescanning a large contract for every match.
	return ,$starts
}

# Frozen validation metadata is not an exception for descriptive strings. Admit
# only original task matches inside two exact ASCII raw-byte regions. Changed
# bytes/offsets fall back to ordinary findings; candidate data remains audited.
function Get-FrozenValidationMetadataStarts([string]$Text, [string]$RelativePath,
	[string]$Kind, $ConfigurationDocument)
{
	$starts = [Collections.Generic.HashSet[int]]::new()
	if($Kind -cne 'content' -or
		'frozenValidationMetadataAllowances' -notin $ConfigurationDocument.PSObject.Properties.Name) { return ,$starts }
	$entries = @($ConfigurationDocument.frozenValidationMetadataAllowances | Where-Object { $_.file -ceq $RelativePath })
	if($entries.Count -ne 1) { return ,$starts }
	$entry = $entries[0]
	if($entry.pattern -cne 'task') { return ,$starts }
	$bytes = [IO.File]::ReadAllBytes((Resolve-RepositoryPath $RelativePath))
	$length = [int]$entry.prefixBytes
	if($length -le 0 -or $length -gt $bytes.Length -or $Text.Length -lt $length) { return ,$starts }
	for($i = 0; $i -lt $length; ++$i) { if($bytes[$i] -gt 127) { return ,$starts } }
	$prefix = [Text.Encoding]::ASCII.GetString($bytes, 0, $length)
	if($Text.Substring(0, $length) -cne $prefix) { return ,$starts }
	if($entry.region -ceq 'whole-file') {
		if($bytes.Length -ne $length) { return ,$starts }
	} elseif($entry.region -ceq 'before-root-candidate') {
		$boundary = [Text.Encoding]::ASCII.GetBytes("`n`t`"candidate`":")
		if($bytes.Length -lt $length + $boundary.Length) { return ,$starts }
		for($i = 0; $i -lt $boundary.Length; ++$i) {
			if($bytes[$length + $i] -ne $boundary[$i]) { return ,$starts }
		}
	} else { return ,$starts }
	$algorithm = [Security.Cryptography.SHA256]::Create()
	try { $hash = ([BitConverter]::ToString($algorithm.ComputeHash($bytes, 0, $length))).Replace('-', '').ToLowerInvariant() }
	finally { $algorithm.Dispose() }
	if($hash -cne $entry.sha256) { return ,$starts }
	foreach($row in $entry.matches) {
		$index = [int]$row.index; $literal = [string]$row.value
		if($index -lt 0 -or $index + $literal.Length -gt $length -or
			$prefix.Substring($index, $literal.Length) -cne $literal -or $starts.Contains($index)) {
			return ,([Collections.Generic.HashSet[int]]::new())
		}
		[void]$starts.Add($index)
	}
	return ,$starts
}

function Add-Violations(
	[Collections.Generic.List[object]]$Violations,
	[string]$RelativePath,
	[string]$Kind,
	[string]$Text,
	$Patterns,
	$ConfigurationDocument)
{
	$frozenStarts = Get-FrozenValidationMetadataStarts $Text $RelativePath $Kind $ConfigurationDocument
	$allowance = $ConfigurationDocument.protocolMetadataAllowance
	$literalStarts = [Collections.Generic.HashSet[int]]::new()
	if($Kind -in @('content', 'encoded-content') -and $RelativePath -cin @($allowance.files))
	{
		$literalStarts = Get-CompleteQuotedLiteralStarts $Text $allowance.literals
	}
	foreach($pattern in $Patterns)
	{
		foreach($match in [regex]::Matches($Text, [string]$pattern.expression))
		{
			# Version-one validation evidence has nine stable wire identifiers. Only
			# an entire quoted identifier in a declared contract file is metadata;
			# filenames, descriptions, unknown identifiers and other rules still fail.
			if($Kind -ceq 'content' -and $pattern.name -ceq 'task' -and $frozenStarts.Contains($match.Index)) { continue }
			$after = $match.Index + $match.Length
			if($Kind -in @('content', 'encoded-content') -and
				$pattern.name -ceq $allowance.pattern -and
				$RelativePath -cin @($allowance.files) -and
				$match.Value -cin @($allowance.literals) -and
				$match.Index -gt 0 -and $after -lt $Text.Length)
			{
				if($literalStarts.Contains($match.Index))
				{
					continue
				}
			}
			$line = 1 + @($Text.Substring(0, $match.Index) -split "`n").Count - 1
			$lineStart = $Text.LastIndexOf("`n", [Math]::Max(0, $match.Index - 1)) + 1
			$Violations.Add([ordered]@{
				path = $RelativePath
				kind = $Kind
				pattern = [string]$pattern.name
				line = $line
				column = 1 + $match.Index - $lineStart
				match = $match.Value
			})
		}
	}
}

function Get-DecodedTextCandidates([string]$Text)
{
	$candidates = [Collections.Generic.List[string]]::new()
	try
	{
		$uriDecoded = [uri]::UnescapeDataString($Text)
		if($uriDecoded -ne $Text)
		{
			$candidates.Add($uriDecoded)
		}
	}
	catch
	{
		# Invalid percent escapes are ordinary product content and remain covered by direct scanning.
	}
	foreach($token in [regex]::Matches($Text, '(?<![A-Za-z0-9+/=])[A-Za-z0-9+/]{20,}={0,2}(?![A-Za-z0-9+/=])'))
	{
		try
		{
			$decoded = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($token.Value))
			if($decoded.Length -gt 0 -and
				(@($decoded.ToCharArray() | Where-Object { [char]::IsControl($_) -and $_ -notin @("`r", "`n", "`t") }).Count -eq 0))
			{
				$candidates.Add($decoded)
			}
		}
		catch
		{
			# A base64-looking token is not evidence unless it decodes as readable text.
		}
	}
	return $candidates
}

$configurationPath = Resolve-RepositoryPath $Configuration
if(-not (Test-Path -LiteralPath $configurationPath -PathType Leaf))
{
	throw "Semantic naming configuration is missing: $configurationPath"
}
$configurationDocument = Get-Content -Raw -LiteralPath $configurationPath |
	ConvertFrom-Json

$files = [Collections.Generic.List[IO.FileInfo]]::new()
foreach($productRoot in @($configurationDocument.productRoots))
{
	$rootPath = Join-Path $repositoryRoot $productRoot
	if(-not (Test-Path -LiteralPath $rootPath -PathType Container))
	{
		throw "Configured product root is missing: $productRoot"
	}
	Get-ChildItem -LiteralPath $rootPath -File -Recurse -Force |
		ForEach-Object { $files.Add($_) }
}
foreach($rootFile in @($configurationDocument.rootFiles))
{
	$rootFilePath = Join-Path $repositoryRoot $rootFile
	if(Test-Path -LiteralPath $rootFilePath -PathType Leaf)
	{
		$files.Add((Get-Item -LiteralPath $rootFilePath))
	}
}

$violations = [Collections.Generic.List[object]]::new()
$scannedFiles = 0
foreach($file in @($files | Sort-Object FullName -Unique))
{
	$relativePath = Get-RepositoryRelativePath $file.FullName
	if(Test-ExcludedPath $relativePath $configurationDocument)
	{
		continue
	}
	++$scannedFiles
	Add-Violations $violations $relativePath 'filename' $relativePath $configurationDocument.patterns $configurationDocument
	# Explicit root files are text even when they have no familiar extension,
	# so contributor guides and repository settings cannot escape this audit.
	if($file.Length -gt [int64]$configurationDocument.maximumTextBytes -or
		($file.Extension -notin @($configurationDocument.textExtensions) -and
			$relativePath -notin @($configurationDocument.rootFiles)))
	{
		continue
	}
	$text = Get-Content -Raw -LiteralPath $file.FullName
	Add-Violations $violations $relativePath 'content' $text $configurationDocument.patterns $configurationDocument
	foreach($decodedText in Get-DecodedTextCandidates $text)
	{
		Add-Violations $violations $relativePath 'encoded-content' $decodedText $configurationDocument.patterns $configurationDocument
	}
}

$report = [ordered]@{
	schemaVersion = 1
	status = if($violations.Count -eq 0) { 'passed' } else { 'failed' }
	repositoryRoot = $repositoryRoot
	scannedFiles = $scannedFiles
	excludedRoots = @($configurationDocument.excludedRoots)
	violations = @($violations)
}
if(-not [string]::IsNullOrWhiteSpace($ReportPath))
{
	$resolvedReportPath = Resolve-RepositoryPath $ReportPath
	[IO.Directory]::CreateDirectory((Split-Path -Parent $resolvedReportPath)) | Out-Null
	$report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $resolvedReportPath -Encoding utf8
}
$report | ConvertTo-Json -Depth 16
if($report.status -ne 'passed' -and -not $NoFail)
{
	exit 1
}
