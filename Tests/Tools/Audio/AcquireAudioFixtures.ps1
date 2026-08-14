<#
.SYNOPSIS
    Acquire and verify the small, pinned Kenney audio fixture subset.

Network access is intentionally limited to maintainer refreshes. Normal builds use
-VerifyCommitted, which downloads to a temporary directory and only compares bytes.
#>
[CmdletBinding()]
param([switch]$VerifyCommitted, [switch]$Refresh)
$ErrorActionPreference = 'Stop'
if (-not $VerifyCommitted -and -not $Refresh) { throw 'Specify -VerifyCommitted or -Refresh.' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$destination = Join-Path $root 'Tests/Fixtures/Audio/ThirdParty/Kenney'
$sources = @(
    @{ Name='Music'; Url='https://kenney.nl/media/pages/assets/music-jingles/f37e530b9e-1677590399/kenney_music-jingles.zip'; Page='https://www.kenney.nl/assets/music-jingles'; Size=1239525; Sha256='b729ba57959bd58793d2c5cafa348aaf2655d354f3da35ec4729e03ec77197b8'; License='Music-LICENSE.txt'; Files=@(@{Archive='Audio/8-Bit jingles/jingles_NES00.ogg'; Relative='Music/jingles_NES00.ogg'; Size=39948; Sha256='1010c9b566d6c090ff03a4aaaf7439e26eb4cc4f20373800d0b7e6a9057b795d'}) }
    @{ Name='Interface-Sounds'; Url='https://kenney.nl/media/pages/assets/interface-sounds/fa43c1dd4d-1677589452/kenney_interface-sounds.zip'; Page='https://www.kenney.nl/assets/interface-sounds'; Size=834536; Sha256='f2193d072726d6758a5f7871b2dcc54dcce0d5c35c6f0a62f92549b327c81232'; License='Interface-Sounds-LICENSE.txt'; Files=@(
        @{Archive='Audio/click_001.ogg'; Relative='Sfx/click_001.ogg'; Size=4876; Sha256='ccfb7fa0cccdd9faec0eb16033c732b1e308d139d80f799161495d58f7adcdb9'}
        @{Archive='Audio/confirmation_001.ogg'; Relative='Sfx/confirmation_001.ogg'; Size=8968; Sha256='063564703b6094d70718a3e787a55cc9141611e4ecd6b6637f8828f79b4a8c3a'}
        @{Archive='Audio/error_001.ogg'; Relative='Sfx/error_001.ogg'; Size=7373; Sha256='46e67425d16339772e8d328fb36a49426c9467418686e11beeb71ff84b0f6433'}
    ) }
)
$temp = Join-Path ([IO.Path]::GetTempPath()) ('terra-audio-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $records = @()
    foreach ($source in $sources) {
        $zip = Join-Path $temp ($source.Name + '.zip')
        Invoke-WebRequest -Uri $source.Url -OutFile $zip -MaximumRedirection 0
        if ((Get-Item $zip).Length -ne $source.Size) { throw "Archive size mismatch: $($source.Name)" }
        if ((Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant() -ne $source.Sha256) { throw "Archive hash mismatch: $($source.Name)" }
        $extract = Join-Path $temp $source.Name; Expand-Archive $zip -DestinationPath $extract
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            $allowed = @($source.Files.Archive) + 'License.txt'
            $seen = [Collections.Generic.HashSet[string]]::new()
            foreach ($entry in $archive.Entries) {
                $name = $entry.FullName.Replace('\','/'); if ($name.EndsWith('/')) { continue }
                if (-not $seen.Add($name)) { throw "Duplicate archive entry: $name" }
                if (($entry.ExternalAttributes -band 0xA0000000) -ne 0) { throw "Symlink entry rejected: $name" }
                if ([IO.Path]::GetFullPath((Join-Path $extract $name)) -notlike (([IO.Path]::GetFullPath($extract)) + '*')) { throw 'Path traversal detected.' }
            }
        } finally { $archive.Dispose() }
        $license = Join-Path $extract 'License.txt'; if (-not (Test-Path $license)) { throw 'Missing License.txt' }
        if ($Refresh) { New-Item -ItemType Directory -Force -Path $destination | Out-Null; Copy-Item $license (Join-Path $destination $source.License) -Force }
        foreach ($file in $source.Files) {
            $actual = Join-Path $extract $file.Archive
            if ((Get-Item $actual).Length -ne $file.Size -or (Get-FileHash $actual -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Sha256) { throw "Fixture mismatch: $($file.Archive)" }
            $target = Join-Path $destination $file.Relative
            if ($Refresh) { New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null; Copy-Item $actual $target -Force }
            elseif (-not (Test-Path $target) -or (Get-FileHash $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Sha256) { throw "Committed fixture mismatch: $($file.Relative)" }
            $records += [ordered]@{ source=$source.Name; sourcePage=$source.Page; archiveUrl=$source.Url; archiveBytes=$source.Size; archiveSha256=$source.Sha256; licenseFile=$source.License; archivePath=$file.Archive; committedPath=('Tests/Fixtures/Audio/ThirdParty/Kenney/' + $file.Relative); bytes=$file.Size; sha256=$file.Sha256 }
        }
    }
    if ($Refresh) { [ordered]@{ schemaVersion=1; license='CC0 1.0'; sources=$records; acquiredUtc=(Get-Date).ToUniversalTime().ToString('o') } | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $destination 'fixtures.json') -Encoding utf8 }
} finally { Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue }
