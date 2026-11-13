[CmdletBinding()]
param([switch]$VerifyCommitted, [switch]$Refresh)
$ErrorActionPreference='Stop'; if (-not $VerifyCommitted -and -not $Refresh) { throw 'Specify -VerifyCommitted or -Refresh.' }
$root=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path; $dest=Join-Path $root 'Tests/Fixtures/Audio/Generated'; $tmp=Join-Path ([IO.Path]::GetTempPath()) ('terra-wav-'+[guid]::NewGuid()); New-Item $tmp -ItemType Directory|Out-Null
function Wav([string]$Path,[int]$Channels,[int]$Rate,[int]$Frames,[int[]]$Samples) {
    $bits=16;$block=$Channels*2;$dataSize=$Frames*$block;$bytes=[IO.MemoryStream]::new();$w=[IO.BinaryWriter]::new($bytes)
    $w.Write([Text.Encoding]::ASCII.GetBytes('RIFF'));$w.Write([int](36+$dataSize));$w.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '));$w.Write(16);$w.Write([int16]1);$w.Write([int16]$Channels);$w.Write($Rate);$w.Write($Rate*$block);$w.Write([int16]$block);$w.Write([int16]$bits);$w.Write([Text.Encoding]::ASCII.GetBytes('data'));$w.Write($dataSize)
    for($i=0;$i -lt $Frames;$i++){for($c=0;$c -lt $Channels;$c++){ $v=if($i*$Channels+$c -lt $Samples.Count){$Samples[$i*$Channels+$c]}else{0};$w.Write([int16]$v)}};$w.Flush();[IO.File]::WriteAllBytes($Path,$bytes.ToArray());$w.Dispose();$bytes.Dispose()
}
try {
    Wav (Join-Path $tmp 'mono_pcm16.wav') 1 8000 8 @(0,1000,-1000,0,500,-500,0,0)
    Wav (Join-Path $tmp 'stereo_impulses.wav') 2 8000 4 @(30000,0,0,30000,0,0,0,0)
    Wav (Join-Path $tmp 'loop_boundary.wav') 1 44100 4 @(1000,1000,-1000,-1000)
    Wav (Join-Path $tmp 'zero_frame.wav') 1 8000 0 @()
    $valid=[IO.File]::ReadAllBytes((Join-Path $tmp 'mono_pcm16.wav')); [IO.File]::WriteAllBytes((Join-Path $tmp 'truncated_header.wav'),$valid[0..11]); [IO.File]::WriteAllBytes((Join-Path $tmp 'truncated_payload.wav'),$valid[0..39])
    [IO.File]::WriteAllBytes((Join-Path $tmp 'invalid_metadata.wav'),[byte[]](0..15)); [IO.File]::WriteAllBytes((Join-Path $tmp 'corrupt_ogg.ogg'),[byte[]](0x4F,0x67,0x67,0x53,0,1,2,3,4,5))
    $manifest=@(); Get-ChildItem $tmp -File | ForEach-Object { $h=Get-FileHash $_.FullName -Algorithm SHA256; $manifest += [ordered]@{file=$_.Name; bytes=$_.Length; sha256=$h.Hash.ToLowerInvariant()} }
    if($Refresh){New-Item $dest -ItemType Directory -Force|Out-Null; Copy-Item (Join-Path $tmp '*') $dest -Force; [ordered]@{schemaVersion=1; fixtures=$manifest}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $dest 'manifest.json') -Encoding utf8}
    else {
		$comm=Get-Content -Raw -LiteralPath (Join-Path $dest 'manifest.json') | ConvertFrom-Json
		if ($comm.schemaVersion -ne 1) { throw 'Unsupported committed audio manifest schema.' }
		$generated=@($manifest | Sort-Object file)
		$committed=@($comm.fixtures | Sort-Object file)
		# Exact inventory includes hidden fixtures, so an extra file cannot hide
		# from the same byte/name checks applied to ordinary committed assets.
		$actualNames=@(Get-ChildItem -LiteralPath $dest -File -Force | Where-Object Name -NE 'manifest.json' | Sort-Object Name | ForEach-Object Name)
		if ($generated.Count -ne $committed.Count -or $actualNames.Count -ne $generated.Count) { throw 'Generated audio fixture inventory differs from committed data.' }
		for ($index=0; $index -lt $generated.Count; ++$index) {
			$fresh=$generated[$index]; $entry=$committed[$index]
			# Names originate in the bounded fresh generator; do not resolve a
			# manifest-supplied traversal path before its exact inventory match.
			if ($fresh.file -cne $entry.file -or $fresh.file -cne $actualNames[$index] -or $fresh.bytes -ne $entry.bytes -or $fresh.sha256 -cne $entry.sha256) { throw "Freshly generated audio manifest mismatch: $($fresh.file)" }
			$path=Join-Path $dest $fresh.file
			if ((Get-Item -LiteralPath $path).Length -ne $fresh.bytes -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $fresh.sha256) { throw "Freshly generated audio bytes mismatch: $($fresh.file)" }
		}
	}
} finally {
	$tempParent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
	$owned=[IO.Path]::GetFullPath($tmp)
	if ([IO.Path]::GetDirectoryName($owned) -eq $tempParent -and [IO.Path]::GetFileName($owned).StartsWith('terra-wav-')) {
		Remove-Item -LiteralPath $owned -Recurse -Force -ErrorAction SilentlyContinue
	}
}
