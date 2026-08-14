[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Baseline,
	[Parameter(Mandatory = $true)][string]$Candidate,
	[Parameter(Mandatory = $true)][string]$Output,
	[int]$MaxDifferentPixels = 0,
	[ValidateRange(0, 255)][int]$PixelChannelTolerance = 0,
	[ValidateRange(0.0, 255.0)][double]$MaxMeanAbsoluteError = 0.0,
	[ValidateRange(0.0, 1.0)][double]$MaxCoverageRatioDelta = 0.0,
	[int]$MaxSilhouetteDifferentPixels = 0,
	[string]$DifferenceImage = '',
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$outputDirectory = Split-Path -Parent $Output
if(-not [string]::IsNullOrWhiteSpace($outputDirectory))
{
	New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}

function Write-VisualRejection([string]$Reason)
{
	[ordered]@{
		schemaVersion = 2
		status = 'insufficient'
		classification = 'insufficient'
		visualFirst = $true
		availability = 'unavailable'
		reason = $Reason
		unavailableIsNotZero = $true
	} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding utf8
}

# Visual claims operate on decoded pixels. Comparing PNG container bytes would
# reject harmless metadata differences and, more importantly, would not measure
# silhouette, coverage, or per-channel image error.
foreach($path in @($Baseline, $Candidate))
{
	if(-not (Test-Path -LiteralPath $path -PathType Leaf))
	{
		Write-VisualRejection "image-artifact-missing:$path"
		exit 2
	}
}
if($DryRun)
{
	Write-Output 'validated image artifacts (dry run; no pixel claim)'
	exit 0
}

try { Add-Type -AssemblyName System.Drawing } catch { Write-VisualRejection 'system-drawing-unavailable'; exit 2 }

function Convert-ToArgbBitmap([string]$Path)
{
	$source = [Drawing.Bitmap]::FromFile((Resolve-Path -LiteralPath $Path).Path)
	try
	{
		$converted = [Drawing.Bitmap]::new(
			$source.Width,
			$source.Height,
			[Drawing.Imaging.PixelFormat]::Format32bppArgb)
		$graphics = [Drawing.Graphics]::FromImage($converted)
		try
		{
			$graphics.DrawImageUnscaled($source, 0, 0)
		}
		finally
		{
			$graphics.Dispose()
		}
		return $converted
	}
	finally
	{
		$source.Dispose()
	}
}

function Get-BitmapBytes([Drawing.Bitmap]$Bitmap)
{
	$rectangle = [Drawing.Rectangle]::new(0, 0, $Bitmap.Width, $Bitmap.Height)
	$data = $Bitmap.LockBits(
		$rectangle,
		[Drawing.Imaging.ImageLockMode]::ReadOnly,
		[Drawing.Imaging.PixelFormat]::Format32bppArgb)
	try
	{
		$bytes = [byte[]]::new([Math]::Abs($data.Stride) * $Bitmap.Height)
		[Runtime.InteropServices.Marshal]::Copy(
			$data.Scan0,
			$bytes,
			0,
			$bytes.Length)
		return [pscustomobject]@{
			Bytes = $bytes
			Stride = [Math]::Abs($data.Stride)
		}
	}
	finally
	{
		$Bitmap.UnlockBits($data)
	}
}

function Test-ForegroundPixel([byte[]]$Bytes, [int]$Offset)
{
	# Alpha is authoritative when present. Opaque captures use luminance so a
	# black clear/background pixel does not become geometry coverage.
	if($Bytes[$Offset + 3] -lt 8)
	{
		return $false
	}
	return ([int]$Bytes[$Offset] +
		[int]$Bytes[$Offset + 1] +
		[int]$Bytes[$Offset + 2]) -gt 12
}

$baselineBitmap = $null
$candidateBitmap = $null
try
{
	$baselineBitmap = Convert-ToArgbBitmap $Baseline
	$candidateBitmap = Convert-ToArgbBitmap $Candidate
}
catch
{
	if($null -ne $baselineBitmap) { $baselineBitmap.Dispose() }
	Write-VisualRejection 'image-artifact-unreadable-or-non-image'
	exit 2
}
try
{
	$dimensionsMatch = $baselineBitmap.Width -eq $candidateBitmap.Width -and
		$baselineBitmap.Height -eq $candidateBitmap.Height
	if(-not $dimensionsMatch)
	{
		$result = [ordered]@{
			schemaVersion = 2
			status = 'mismatch'
			classification = 'visual-mismatch'
			visualFirst = $true
			availability = 'measured'
			dimensionsMatched = $false
			baselineDimensions = @($baselineBitmap.Width, $baselineBitmap.Height)
			candidateDimensions = @($candidateBitmap.Width, $candidateBitmap.Height)
			baselineSha256 = (Get-FileHash -LiteralPath $Baseline -Algorithm SHA256).Hash.ToLowerInvariant()
			candidateSha256 = (Get-FileHash -LiteralPath $Candidate -Algorithm SHA256).Hash.ToLowerInvariant()
			note = 'Decoded image dimensions differ; performance classification is blocked.'
		}
		$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding utf8
		exit 2
	}

	$baselinePixels = Get-BitmapBytes $baselineBitmap
	$candidatePixels = Get-BitmapBytes $candidateBitmap
	$width = $baselineBitmap.Width
	$height = $baselineBitmap.Height
	$pixelCount = [int64]$width * [int64]$height
	$differentPixels = 0L
	$silhouetteDifferentPixels = 0L
	$baselineCoverage = 0L
	$candidateCoverage = 0L
	$absoluteError = 0L
	$maximumChannelDelta = 0
	$differenceBitmap = [Drawing.Bitmap]::new(
		$width,
		$height,
		[Drawing.Imaging.PixelFormat]::Format32bppArgb)
	try
	{
		for($y = 0; $y -lt $height; ++$y)
		{
			for($x = 0; $x -lt $width; ++$x)
			{
				$baselineOffset = $y * $baselinePixels.Stride + $x * 4
				$candidateOffset = $y * $candidatePixels.Stride + $x * 4
				$pixelDifferent = $false
				for($channel = 0; $channel -lt 4; ++$channel)
				{
					$delta = [Math]::Abs(
						[int]$baselinePixels.Bytes[$baselineOffset + $channel] -
						[int]$candidatePixels.Bytes[$candidateOffset + $channel])
					$absoluteError += $delta
					$maximumChannelDelta = [Math]::Max($maximumChannelDelta, $delta)
					if($delta -gt $PixelChannelTolerance)
					{
						$pixelDifferent = $true
					}
				}

				$baselineForeground = Test-ForegroundPixel $baselinePixels.Bytes $baselineOffset
				$candidateForeground = Test-ForegroundPixel $candidatePixels.Bytes $candidateOffset
				if($baselineForeground) { ++$baselineCoverage }
				if($candidateForeground) { ++$candidateCoverage }
				if($baselineForeground -ne $candidateForeground)
				{
					++$silhouetteDifferentPixels
				}
				if($pixelDifferent)
				{
					++$differentPixels
					$differenceBitmap.SetPixel($x, $y, [Drawing.Color]::FromArgb(255, 255, 0, 0))
				}
				else
				{
					$grey = [byte](([int]$baselinePixels.Bytes[$baselineOffset] +
						[int]$baselinePixels.Bytes[$baselineOffset + 1] +
						[int]$baselinePixels.Bytes[$baselineOffset + 2]) / 6)
					$differenceBitmap.SetPixel($x, $y, [Drawing.Color]::FromArgb(255, $grey, $grey, $grey))
				}
			}
		}

		$meanAbsoluteError = if($pixelCount -eq 0L)
			{ 0.0 }
			else { [double]$absoluteError / ([double]$pixelCount * 4.0) }
		$baselineCoverageRatio = if($pixelCount -eq 0L)
			{ 0.0 }
			else { [double]$baselineCoverage / [double]$pixelCount }
		$candidateCoverageRatio = if($pixelCount -eq 0L)
			{ 0.0 }
			else { [double]$candidateCoverage / [double]$pixelCount }
		$coverageRatioDelta = [Math]::Abs(
			$baselineCoverageRatio - $candidateCoverageRatio)
		$accepted = $differentPixels -le $MaxDifferentPixels -and
			$meanAbsoluteError -le $MaxMeanAbsoluteError -and
			$coverageRatioDelta -le $MaxCoverageRatioDelta -and
			$silhouetteDifferentPixels -le $MaxSilhouetteDifferentPixels

		if([string]::IsNullOrWhiteSpace($DifferenceImage))
		{
			$DifferenceImage = "$Output.diff.png"
		}
		$differenceDirectory = Split-Path -Parent $DifferenceImage
		if(-not [string]::IsNullOrWhiteSpace($differenceDirectory))
		{
			New-Item -ItemType Directory -Force -Path $differenceDirectory | Out-Null
		}
		$differenceBitmap.Save(
			[IO.Path]::GetFullPath($DifferenceImage),
			[Drawing.Imaging.ImageFormat]::Png)

		$result = [ordered]@{
			schemaVersion = 2
			status = if($accepted) { 'accepted' } else { 'mismatch' }
			classification = if($accepted) { 'visual-parity' } else { 'visual-mismatch' }
			visualFirst = $true
			availability = 'measured'
			dimensionsMatched = $true
			width = $width
			height = $height
			pixelCount = $pixelCount
			differentPixels = $differentPixels
			maxDifferentPixels = $MaxDifferentPixels
			pixelChannelTolerance = $PixelChannelTolerance
			meanAbsoluteError = $meanAbsoluteError
			maxMeanAbsoluteError = $MaxMeanAbsoluteError
			maximumChannelDelta = $maximumChannelDelta
			baselineCoverageRatio = $baselineCoverageRatio
			candidateCoverageRatio = $candidateCoverageRatio
			coverageRatioDelta = $coverageRatioDelta
			maxCoverageRatioDelta = $MaxCoverageRatioDelta
			silhouetteDifferentPixels = $silhouetteDifferentPixels
			maxSilhouetteDifferentPixels = $MaxSilhouetteDifferentPixels
			checks = [ordered]@{
				imageTolerance = $differentPixels -le $MaxDifferentPixels -and
					$meanAbsoluteError -le $MaxMeanAbsoluteError
				silhouette = $silhouetteDifferentPixels -le
					$MaxSilhouetteDifferentPixels
				coverage = $coverageRatioDelta -le $MaxCoverageRatioDelta
				# These checks require view/capture and identity evidence from
				# the installed runtime, not inference from a single image pair.
				material = $null
				shadow = $null
				animation = $null
				crackFree = $null
				identity = $null
			}
			differenceImage = [IO.Path]::GetFullPath($DifferenceImage)
			baselineSha256 = (Get-FileHash -LiteralPath $Baseline -Algorithm SHA256).Hash.ToLowerInvariant()
			candidateSha256 = (Get-FileHash -LiteralPath $Candidate -Algorithm SHA256).Hash.ToLowerInvariant()
			note = 'Decoded ARGB pixels, foreground coverage, and silhouette occupancy were compared before performance classification.'
		}
		$outputDirectory = Split-Path -Parent $Output
		if(-not [string]::IsNullOrWhiteSpace($outputDirectory))
		{
			New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
		}
		$result | ConvertTo-Json -Depth 8 |
			Set-Content -LiteralPath $Output -Encoding utf8
		if(-not $accepted)
		{
			exit 2
		}
	}
	finally
	{
		$differenceBitmap.Dispose()
	}
}
finally
{
	$baselineBitmap.Dispose()
	$candidateBitmap.Dispose()
}
