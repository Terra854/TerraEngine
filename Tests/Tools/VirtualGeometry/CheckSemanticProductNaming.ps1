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

function Add-Violations(
	[Collections.Generic.List[object]]$Violations,
	[string]$RelativePath,
	[string]$Kind,
	[string]$Text,
	$Patterns)
{
	foreach($pattern in $Patterns)
	{
		foreach($match in [regex]::Matches($Text, [string]$pattern.expression))
		{
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
	Add-Violations $violations $relativePath 'filename' $relativePath $configurationDocument.patterns
	if($file.Length -gt [int64]$configurationDocument.maximumTextBytes -or
		$file.Extension -notin @($configurationDocument.textExtensions))
	{
		continue
	}
	$text = Get-Content -Raw -LiteralPath $file.FullName
	Add-Violations $violations $relativePath 'content' $text $configurationDocument.patterns
	foreach($decodedText in Get-DecodedTextCandidates $text)
	{
		Add-Violations $violations $relativePath 'encoded-content' $decodedText $configurationDocument.patterns
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
