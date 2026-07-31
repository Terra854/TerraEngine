# AcquireValidationModels.ps1 - Explicit, fail-closed real-model acquisition.
#
# This script is intentionally never called by CMake, CTest, editor startup,
# runtime startup, or export. It only writes to a disposable validation cache.
# Its -DryRun mode checks manifest/cache safety without opening the network.

[CmdletBinding()]
param(
	[ValidateNotNullOrEmpty()][string]$Manifest = 'Tools/Animation/ValidationModels.json',
	[ValidateNotNullOrEmpty()][string]$CacheRoot = 'out/validation/animation-models',
	[ValidateRange(1, 64)][int]$MaximumEntryCount = 16,
	[ValidateRange(1MB, 8GB)][Int64]$MaximumArchiveBytes = 2GB,
	[ValidateRange(1, 4096)][int]$MaximumExtractedFileCount = 1024,
	[ValidateRange(1MB, 32GB)][Int64]$MaximumExtractedBytes = 8GB,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:ToolVersion = '1.1'

function Get-AbsolutePath([string]$Path, [string]$Base) {
	if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
	return [System.IO.Path]::GetFullPath((Join-Path $Base $Path))
}

function Assert-AtOrBelow([string]$Parent, [string]$Child, [string]$Description) {
	$root = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
	$value = [System.IO.Path]::GetFullPath($Child).TrimEnd('\', '/')
	if ($value.Equals($root, [System.StringComparison]::OrdinalIgnoreCase)) { return }
	if (-not $value.StartsWith($root + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
		throw "$Description must remain below '$root'."
	}
}

function Write-Json([string]$Path, $Value) {
	[System.IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
	[System.IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 64) + "`n"), $script:Utf8NoBom)
}

function Get-Sha256([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-TextSha256([string]$Text) {
	$sha = [System.Security.Cryptography.SHA256]::Create()
	try {
		# BitConverter keeps the tool compatible with the .NET Framework runtime
		# used by Windows PowerShell 5.1, which has no Convert.ToHexString API.
		return ([BitConverter]::ToString($sha.ComputeHash($script:Utf8NoBom.GetBytes($Text))).Replace('-', '')).ToLowerInvariant()
	}
	finally { $sha.Dispose() }
}
function Get-Property($Object, [string]$Name) {
	if ($null -eq $Object -or $null -eq $Object.PSObject.Properties[$Name]) { return $null }
	return $Object.PSObject.Properties[$Name].Value
}
function Assert-Hash([string]$Hash, [string]$Description) {
	if ($Hash -notmatch '^[0-9a-f]{64}$' -or $Hash -match '^0{64}$') { throw "$Description must be a verified, non-placeholder lowercase SHA-256 value." }
}
function Assert-RelativeArchivePath([string]$Path, [string]$Description) {
	if ([string]::IsNullOrWhiteSpace($Path) -or [System.IO.Path]::IsPathRooted($Path) -or $Path.Contains(':') -or $Path -match '(^|[\\/])\.\.([\\/]|$)') { throw "$Description is not a safe relative archive path." }
	return $Path.Replace('\', '/')
}
function Test-ExternalUnavailable($Exception) {
	# Only transport/source outages may be skipped. Integrity, path, and license
	# errors stay hard failures because accepting them would weaken provenance.
	$current = $Exception
	while ($null -ne $current) {
		# Windows PowerShell 5.1 does not always preload System.Net.Http. Compare
		# that exception by its stable type name so outage classification itself
		# cannot hide the original failure on hosts where the assembly is absent.
		if ($current -is [System.Net.WebException] -or $current -is [System.Net.Sockets.SocketException] -or $current.GetType().FullName -eq 'System.Net.Http.HttpRequestException') { return $true }
		$current = $current.InnerException
	}
	return $false
}

function Assert-Manifest($Document) {
	if ([int](Get-Property $Document 'schemaVersion') -ne 1 -or [string](Get-Property $Document 'feature') -ne 'complete-animation-system') { throw 'Manifest has an unsupported identity.' }
	$entries = @((Get-Property $Document 'models'))
	if ($entries.Count -lt 1 -or $entries.Count -gt $MaximumEntryCount) { throw "Manifest model count must be 1..$MaximumEntryCount." }
	$ids = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
	foreach ($entry in $entries) {
		$id = [string](Get-Property $entry 'id')
		if ($id -notmatch '^[a-z0-9][a-z0-9-]{1,95}$' -or -not $ids.Add($id)) { throw "Manifest has an unsafe or duplicate model id '$id'." }
		if ([string](Get-Property $entry 'verificationStatus') -ne 'verified') { throw "Model '$id' is not independently verified; acquisition is blocked." }
		$url = [string](Get-Property $entry 'sourceUrl')
		if ($url -notmatch '^https://') { throw "Model '$id' sourceUrl must use HTTPS." }
		$sourceKind = [string](Get-Property $entry 'sourceKind')
		if ($sourceKind -notin @('file', 'zip')) { throw "Model '$id' sourceKind must be 'file' or 'zip'." }
		$commit = [string](Get-Property $entry 'repositoryCommit')
		if (-not [string]::IsNullOrWhiteSpace($commit) -and $commit -notmatch '^[0-9a-f]{40,64}$') { throw "Model '$id' repositoryCommit must be a full immutable commit." }
		Assert-Hash ([string](Get-Property $entry 'archiveSha256')) "Model '$id' archiveSha256"
		if ([string]::IsNullOrWhiteSpace([string](Get-Property $entry 'license')) -or [string](Get-Property $entry 'licenseUrl') -notmatch '^https://') { throw "Model '$id' lacks a verifiable license declaration." }
		Assert-Hash ([string](Get-Property $entry 'licenseSha256')) "Model '$id' licenseSha256"
		$licenseFileName = Assert-RelativeArchivePath ([string](Get-Property $entry 'licenseFileName')) "Model '$id' license file name"
		if ($licenseFileName.Contains('/')) { throw "Model '$id' licenseFileName must be a file name, not a path." }
		if ([string]::IsNullOrWhiteSpace([string](Get-Property $entry 'attribution')) -or @((Get-Property $entry 'purposes')).Count -eq 0 -or @((Get-Property $entry 'expectedFeatures')).Count -eq 0 -or @((Get-Property $entry 'validationScenarios')).Count -eq 0) { throw "Model '$id' lacks required attribution, tags, features, or scenarios." }
		$paths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
		$files = @((Get-Property $entry 'files'))
		if ($files.Count -lt 1) { throw "Model '$id' must pin at least one expected extracted file." }
		foreach ($file in $files) { $safe = Assert-RelativeArchivePath ([string](Get-Property $file 'path')) "Model '$id' file path"; if (-not $paths.Add($safe)) { throw "Model '$id' has duplicate case-folded file '$safe'." }; Assert-Hash ([string](Get-Property $file 'sha256')) "Model '$id' file '$safe' hash" }
		if ($sourceKind -eq 'file' -and $files.Count -ne 1) { throw "Model '$id' single-file source must declare exactly one payload file." }
	}
	return $entries
}

function Publish-StagedEntry([string]$CacheRoot, [string]$Staging, [string]$EntryRoot) {
	# Keep the previous verified cache entry recoverable until the complete new
	# staging tree has been promoted. This preserves last-known-good provenance
	# if a filesystem error occurs during replacement.
	$backup = "$EntryRoot.previous-$([guid]::NewGuid().ToString('N'))"
	Assert-AtOrBelow $CacheRoot $backup 'Model cache backup'
	$hadPrevious = Test-Path -LiteralPath $EntryRoot
	try {
		if ($hadPrevious) { Move-Item -LiteralPath $EntryRoot -Destination $backup }
		Move-Item -LiteralPath $Staging -Destination $EntryRoot
		if ($hadPrevious -and (Test-Path -LiteralPath $backup)) { Remove-Item -LiteralPath $backup -Recurse -Force }
	}
	catch {
		if (-not (Test-Path -LiteralPath $EntryRoot) -and (Test-Path -LiteralPath $backup)) { Move-Item -LiteralPath $backup -Destination $EntryRoot }
		throw
	}
}

function Expand-SafeZip([string]$Archive, [string]$Destination) {
	Add-Type -AssemblyName System.IO.Compression.FileSystem
	$zip = [System.IO.Compression.ZipFile]::OpenRead($Archive)
	try {
		if ($zip.Entries.Count -gt $MaximumExtractedFileCount) { throw 'Archive exceeds the extracted file-count limit.' }
		$seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase); [Int64]$total = 0
		$allowedExtensions = @('.gltf', '.glb', '.bin', '.fbx', '.obj', '.mtl', '.blend', '.png', '.jpg', '.jpeg', '.webp', '.ktx', '.ktx2', '.txt', '.md', '.pdf', '.license')
		foreach ($item in $zip.Entries) {
			if ([string]::IsNullOrEmpty($item.Name)) { continue }
			$relative = Assert-RelativeArchivePath $item.FullName 'Archive entry'
			if (-not $seen.Add($relative)) { throw "Archive has duplicate case-folded path '$relative'." }
			# Unix symlinks encode their type in the upper external-attribute bits.
			# Rejecting every non-regular file prevents an archive link escaping cache.
			$unixType = (($item.ExternalAttributes -shr 16) -band 0xF000)
			if ($unixType -ne 0 -and $unixType -ne 0x8000) { throw "Archive entry '$relative' is not a regular file." }
			if ($allowedExtensions -notcontains [System.IO.Path]::GetExtension($relative).ToLowerInvariant()) { throw "Archive entry '$relative' has an unexpected file type." }
			if ($item.Length -gt $MaximumExtractedBytes -or $total -gt ($MaximumExtractedBytes - $item.Length)) { throw 'Archive exceeds the extracted size limit.' }; $total += $item.Length
			$target = Join-Path $Destination $relative; Assert-AtOrBelow $Destination $target 'Archive extraction target'
			[System.IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
			# ExtractToFile cannot follow archive symlinks; regular ZIP entries only are accepted.
			[System.IO.Compression.ZipFileExtensions]::ExtractToFile($item, $target, $false)
		}
	}
	finally { $zip.Dispose() }
}

$repoRoot = (Get-Location).Path
$manifestPath = Get-AbsolutePath $Manifest $repoRoot
$cachePath = Get-AbsolutePath $CacheRoot $repoRoot
$result = [ordered]@{ schemaVersion = 1; tool = 'AcquireValidationModels'; task = 'AnimationValidationTraversalSafeSizeCountBoundedHashLicenseVerifyingProvenanceRecording'; feature = 'complete-animation-system'; mode = if ($DryRun) { 'dry-run' } else { 'execute' }; status = 'failed'; failClosed = $true; cacheRoot = $cachePath; models = @(); reason = '' }
try {
	Assert-AtOrBelow (Join-Path $repoRoot 'out') $cachePath 'Validation model cache root'
	if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Manifest is missing: '$manifestPath'." }
	$rawManifest = [System.IO.File]::ReadAllText($manifestPath, $script:Utf8NoBom); $document = $rawManifest | ConvertFrom-Json
	$entries = Assert-Manifest $document
	if ($DryRun) { $result.status = 'ready-for-explicit-acquisition'; $result.models = @($entries | ForEach-Object { [string]$_.id }); $result.reason = 'Manifest and cache constraints are valid; no network request was made.'; $result }
	else {
		[System.IO.Directory]::CreateDirectory($cachePath) | Out-Null
		foreach ($entry in $entries) {
			$id = [string]$entry.id; $entryRoot = Join-Path $cachePath $id; Assert-AtOrBelow $cachePath $entryRoot 'Model cache entry'
			$staging = "$entryRoot.staging-$([guid]::NewGuid().ToString('N'))"; Assert-AtOrBelow $cachePath $staging 'Model staging directory'
			try {
				[System.IO.Directory]::CreateDirectory($staging) | Out-Null
				$sourceKind = [string]$entry.sourceKind
				$sourceExtension = if ($sourceKind -eq 'zip') { '.zip' } else { [System.IO.Path]::GetExtension([string]$entry.files[0].path) }
				$source = Join-Path $staging "source$sourceExtension"
				$download = Invoke-WebRequest -UseBasicParsing -Uri ([uri]$entry.sourceUrl) -OutFile $source -MaximumRedirection 5
				if ((Get-Item -LiteralPath $source).Length -gt $MaximumArchiveBytes) { throw "Model '$id' source artifact exceeds the bounded download size." }
				$archiveHash = Get-Sha256 $source; if ($archiveHash -cne [string]$entry.archiveSha256) { throw "Model '$id' source artifact SHA-256 mismatch." }
				$payload = Join-Path $staging 'payload'; [System.IO.Directory]::CreateDirectory($payload) | Out-Null
				if ($sourceKind -eq 'zip') {
					Expand-SafeZip $source $payload
				}
				else {
					$payloadFile = Join-Path $payload ([string]$entry.files[0].path)
					Assert-AtOrBelow $payload $payloadFile 'Single-file payload target'
					[System.IO.Directory]::CreateDirectory((Split-Path -Parent $payloadFile)) | Out-Null
					Copy-Item -LiteralPath $source -Destination $payloadFile
				}
				foreach ($expected in @($entry.files)) { $actual = Join-Path $payload ([string]$expected.path); Assert-AtOrBelow $payload $actual 'Expected payload file'; if (-not (Test-Path -LiteralPath $actual -PathType Leaf) -or (Get-Sha256 $actual) -cne [string]$expected.sha256) { throw "Model '$id' expected file '$($expected.path)' is missing or has the wrong SHA-256." } }
				# License evidence is downloaded from the separately declared, model-
				# specific authoritative source and hash checked. It is never inferred
				# from a repository-wide license or from the downloaded model bytes.
				$licenseRoot = Join-Path $staging 'licenses'; [System.IO.Directory]::CreateDirectory($licenseRoot) | Out-Null
				$licenseFile = Join-Path $licenseRoot ([string]$entry.licenseFileName); Assert-AtOrBelow $licenseRoot $licenseFile 'License evidence target'
				$licenseDownload = Invoke-WebRequest -UseBasicParsing -Uri ([uri]$entry.licenseUrl) -OutFile $licenseFile -MaximumRedirection 5
				if ((Get-Item -LiteralPath $licenseFile).Length -gt 16MB) { throw "Model '$id' license evidence exceeds the bounded size." }
				$licenseHash = Get-Sha256 $licenseFile; if ($licenseHash -cne [string]$entry.licenseSha256) { throw "Model '$id' license evidence SHA-256 mismatch." }
				$resolvedUrl = if ($null -ne $download -and $null -ne $download.BaseResponse) { $download.BaseResponse.ResponseUri.AbsoluteUri } else { ([uri]$entry.sourceUrl).AbsoluteUri }
				$resolvedLicenseUrl = if ($null -ne $licenseDownload -and $null -ne $licenseDownload.BaseResponse) { $licenseDownload.BaseResponse.ResponseUri.AbsoluteUri } else { ([uri]$entry.licenseUrl).AbsoluteUri }
				$provenance = [ordered]@{ schemaVersion = 1; modelId = $id; acquiredUtc = [DateTime]::UtcNow.ToString('o'); toolVersion = $script:ToolVersion; sourceKind = $sourceKind; resolvedSourceUrl = $resolvedUrl; archiveSha256 = $archiveHash; manifestEntrySha256 = Get-TextSha256 (($entry | ConvertTo-Json -Depth 64 -Compress)); files = @($entry.files); copiedLicenseFiles = @([string]$entry.licenseFileName); licenseSha256 = $licenseHash; resolvedLicenseUrl = $resolvedLicenseUrl; license = $entry.license; licenseUrl = $entry.licenseUrl; attribution = $entry.attribution }
				Write-Json (Join-Path $staging 'provenance.json') $provenance
				Publish-StagedEntry $cachePath $staging $entryRoot
				$result.models += [ordered]@{ id = $id; status = 'acquired'; provenance = (Join-Path $entryRoot 'provenance.json') }
			}
			catch { if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }; throw }
		}
		$result.status = 'acquired'; $result.reason = 'All model artifacts, payload files, authoritative license records, and provenance verified.'; $result
	}
}
catch {
	$result.reason = $_.Exception.Message
	if (-not $DryRun -and (Test-ExternalUnavailable $_.Exception)) {
		$result.status = 'SkippedExternalValidation'; $result.reason = "External source unavailable: $($result.reason)"; $result | ConvertTo-Json -Depth 32; exit 0
	}
	$result | ConvertTo-Json -Depth 32; exit 1
}
