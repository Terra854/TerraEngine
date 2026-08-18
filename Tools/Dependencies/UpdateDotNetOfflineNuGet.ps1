param(
	[string]$SdkVersion = "10.0.303",
	[string]$RuntimePackVersion = "10.0.11"
)

# This script refreshes only the versioned .NET runtime and NativeAOT packages
# already approved by TerraEngine's lock manifest. Run it from any directory
# with Windows PowerShell 5.1+ and network access to api.nuget.org.
$ErrorActionPreference = "Stop"
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$lockPath = Join-Path $repositoryRoot "cmake/TerraEngineNuGet.lock.json"
$nugetRoot = Join-Path $repositoryRoot ".nuget"
$feedRoot = Join-Path $nugetRoot "feed"
$licenseRoot = Join-Path $nugetRoot "licenses"
$utf8NoBom = [Text.UTF8Encoding]::new($false)

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-FileHashString {
	param(
		[Parameter(Mandatory)] [string]$Path,
		[Parameter(Mandatory)] [ValidateSet("SHA256", "SHA512")] [string]$Algorithm
	)

	return (Get-FileHash -LiteralPath $Path -Algorithm $Algorithm).Hash.ToLowerInvariant()
}

function Read-ZipEntryBytes {
	param(
		[Parameter(Mandatory)] [IO.Compression.ZipArchive]$Archive,
		[Parameter(Mandatory)] [string]$EntryName
	)

	$entry = $Archive.Entries |
		Where-Object { $_.FullName -eq $EntryName } |
		Select-Object -First 1
	if ($null -eq $entry) {
		throw "Archive entry '$EntryName' was not found."
	}

	$entryStream = $entry.Open()
	$memory = [IO.MemoryStream]::new()
	try {
		$entryStream.CopyTo($memory)
		return $memory.ToArray()
	}
	finally {
		$entryStream.Dispose()
		$memory.Dispose()
	}
}

function Write-LegalFile {
	param(
		[Parameter(Mandatory)] [byte[]]$Bytes,
		[Parameter(Mandatory)] [string]$RelativePath
	)

	$destination = [IO.Path]::GetFullPath((Join-Path $nugetRoot $RelativePath))
	if (-not $destination.StartsWith($licenseRoot, [StringComparison]::OrdinalIgnoreCase)) {
		throw "Refusing to write a legal file outside '$licenseRoot': $destination"
	}
	[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
	[IO.File]::WriteAllBytes($destination, $Bytes)
	return $destination
}

function Remove-StaleGeneratedFiles {
	param([Parameter(Mandatory)] $Manifest)

	$expectedPackages = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach ($package in $Manifest.packages) {
		$expectedPackages.Add([IO.Path]::GetFullPath((Join-Path $nugetRoot ([string]$package.file)))) | Out-Null
	}
	foreach ($archive in Get-ChildItem -LiteralPath $feedRoot -Filter *.nupkg -File) {
		$archivePath = [IO.Path]::GetFullPath($archive.FullName)
		if (-not $archivePath.StartsWith($feedRoot, [StringComparison]::OrdinalIgnoreCase)) {
			throw "Refusing to remove an archive outside '$feedRoot': $archivePath"
		}
		if (-not $expectedPackages.Contains($archivePath)) {
			Remove-Item -LiteralPath $archivePath -Force
		}
	}

	$expectedLegalFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach ($generatedFile in $Manifest.generatedLegalFiles) {
		$expectedLegalFiles.Add([IO.Path]::GetFullPath((Join-Path $nugetRoot ([string]$generatedFile.path)))) | Out-Null
	}
	foreach ($package in $Manifest.packages) {
		foreach ($legalFile in $package.legalFiles) {
			$expectedLegalFiles.Add([IO.Path]::GetFullPath((Join-Path $nugetRoot ([string]$legalFile.path)))) | Out-Null
		}
	}
	foreach ($legalFile in Get-ChildItem -LiteralPath $licenseRoot -Recurse -File) {
		$legalPath = [IO.Path]::GetFullPath($legalFile.FullName)
		if (-not $legalPath.StartsWith($licenseRoot, [StringComparison]::OrdinalIgnoreCase)) {
			throw "Refusing to remove a legal file outside '$licenseRoot': $legalPath"
		}
		if (-not $expectedLegalFiles.Contains($legalPath)) {
			Remove-Item -LiteralPath $legalPath -Force
		}
	}

	# Empty directories are removed from deepest to shallowest. The operation is
	# non-recursive, so a directory containing any unexpected data is preserved.
	Get-ChildItem -LiteralPath $licenseRoot -Recurse -Directory |
		Sort-Object { $_.FullName.Length } -Descending |
		ForEach-Object {
			if (@(Get-ChildItem -LiteralPath $_.FullName -Force).Count -eq 0) {
				Remove-Item -LiteralPath $_.FullName
			}
		}
}

$manifest = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
$oldRuntimePackVersion = [string]$manifest.runtimePackVersion
if ($oldRuntimePackVersion -eq $RuntimePackVersion -and
	[string]$manifest.dotnetSdkVersion -eq $SdkVersion) {
	Remove-StaleGeneratedFiles -Manifest $manifest
	Write-Host "The offline NuGet manifest already pins SDK $SdkVersion and runtime packs $RuntimePackVersion."
	exit 0
}

[IO.Directory]::CreateDirectory($feedRoot) | Out-Null
$updatedPackages = @{}

foreach ($package in $manifest.packages) {
	if ([string]$package.version -ne $oldRuntimePackVersion) {
		continue
	}

	$packageId = [string]$package.id
	$normalizedId = $packageId.ToLowerInvariant()
	$fileName = "$normalizedId.$RuntimePackVersion.nupkg"
	$relativePackagePath = "feed/$fileName"
	$sourceUrl = "https://api.nuget.org/v3-flatcontainer/$normalizedId/$RuntimePackVersion/$fileName"
	$destination = Join-Path $feedRoot $fileName
	$temporaryDownload = "$destination.download"

	# Download to a sibling temporary file so an interrupted refresh never leaves
	# a partial archive at the path consumed by the fail-closed CMake verifier.
	Invoke-WebRequest -Uri $sourceUrl -OutFile $temporaryDownload
	Move-Item -LiteralPath $temporaryDownload -Destination $destination -Force

	$package.version = $RuntimePackVersion
	$package.file = $relativePackagePath
	$package.sourceUrl = $sourceUrl
	$package.bytes = (Get-Item -LiteralPath $destination).Length
	$package.sha256 = Get-FileHashString -Path $destination -Algorithm SHA256
	$package.sha512 = Get-FileHashString -Path $destination -Algorithm SHA512

	$archive = [IO.Compression.ZipFile]::OpenRead($destination)
	try {
		$package.signatureEntryPresent = $null -ne ($archive.Entries |
			Where-Object { $_.FullName -eq ".signature.p7s" } |
			Select-Object -First 1)

		foreach ($legalFile in $package.legalFiles) {
			$oldLegalPath = [string]$legalFile.path
			if ($oldLegalPath -eq "licenses/shared/MIT.txt") {
				continue
			}

			$newLegalPath = $oldLegalPath.Replace("/$oldRuntimePackVersion/", "/$RuntimePackVersion/")
			$fileNameOnly = [IO.Path]::GetFileName($oldLegalPath)
			$archiveEntry = $archive.Entries |
				Where-Object { $_.Name -eq $fileNameOnly } |
				Sort-Object { $_.FullName.Length } |
				Select-Object -First 1
			if ($null -eq $archiveEntry) {
				throw "Package '$packageId' does not contain expected legal file '$fileNameOnly'."
			}

			$bytes = Read-ZipEntryBytes -Archive $archive -EntryName $archiveEntry.FullName
			$writtenPath = Write-LegalFile -Bytes $bytes -RelativePath $newLegalPath
			$legalFile.path = $newLegalPath
			$legalFile.bytes = $bytes.Length
			$legalFile.sha256 = Get-FileHashString -Path $writtenPath -Algorithm SHA256
		}

		if ($null -ne $package.license.primaryFile) {
			$package.license.primaryFile = ([string]$package.license.primaryFile).Replace(
				"/$oldRuntimePackVersion/", "/$RuntimePackVersion/")
		}
	}
	finally {
		$archive.Dispose()
	}

	$updatedPackages[$relativePackagePath] = $destination
}

foreach ($generatedFile in $manifest.generatedLegalFiles) {
	$generatedFile.packageFile = ([string]$generatedFile.packageFile).Replace(
		".$oldRuntimePackVersion.nupkg", ".$RuntimePackVersion.nupkg")
	$packagePath = [string]$generatedFile.packageFile
	$archivePath = if ($updatedPackages.ContainsKey($packagePath)) {
		$updatedPackages[$packagePath]
	} else {
		Join-Path $nugetRoot $packagePath
	}

	$archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
	try {
		$bytes = Read-ZipEntryBytes -Archive $archive -EntryName ([string]$generatedFile.archiveEntry)
		$writtenPath = Write-LegalFile -Bytes $bytes -RelativePath ([string]$generatedFile.path)
		$generatedFile.bytes = $bytes.Length
		$generatedFile.sha256 = Get-FileHashString -Path $writtenPath -Algorithm SHA256
	}
	finally {
		$archive.Dispose()
	}
}

$manifest.dotnetSdkVersion = $SdkVersion
$manifest.runtimePackVersion = $RuntimePackVersion
$manifest.packageCount = @($manifest.packages).Count
$manifest.totalBytes = ($manifest.packages | Measure-Object -Property bytes -Sum).Sum

# ConvertTo-Json provides deterministic property ordering for the existing
# PSCustomObject graph; the lock remains reviewable and CMake consumes JSON
# semantically, independent of whitespace.
$serialized = $manifest | ConvertTo-Json -Depth 20
[IO.File]::WriteAllText($lockPath, "$serialized`n", $utf8NoBom)
Remove-StaleGeneratedFiles -Manifest $manifest
Write-Host "Updated the offline NuGet manifest to SDK $SdkVersion and runtime packs $RuntimePackVersion."
