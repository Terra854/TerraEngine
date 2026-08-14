# VerifyInstalledAudioProviderContract.ps1 - compile the public provider seam.
[CmdletBinding()]
param(
	[string]$IncludeRoot = 'out/build/RelWithDebInfo/Build/include',
	[string]$LibraryPath = '',
	[string]$RuntimeDirectory = ''
)
$ErrorActionPreference='Stop'
if(!(Test-Path $IncludeRoot)){throw "Install include root not found: $IncludeRoot"}
foreach($name in 'miniaudio.h','stb_vorbis.c','MiniaudioBackend.h'){
	if(Get-ChildItem $IncludeRoot -Recurse -Filter $name -ErrorAction SilentlyContinue){throw "Private provider file leaked: $name"}
}
$temporary = Join-Path ([System.IO.Path]::GetTempPath()) ("TerraAudioProvider-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
	$source = Join-Path $temporary 'consumer.cpp'
	@'
#include <TerraEngine/Audio/Backend/IAudioBackend.h>
class ExternalAudioProvider final : public TerraEngine::Audio::Backend::IAudioBackend {
public:
	TerraEngine::Audio::AudioResult Initialize(const TerraEngine::Audio::AudioBackendCreateInfo&) override { return TerraEngine::Audio::AudioResult::Accepted(); }
	TerraEngine::Audio::AudioResult SubmitFrame(const TerraEngine::Audio::AudioBackendFramePacket&) override { return TerraEngine::Audio::AudioResult::Accepted(); }
	TerraEngine::Audio::AudioResult PollObservations(std::span<TerraEngine::Audio::AudioBackendObservation>, std::size_t& required) override { required = 0; return TerraEngine::Audio::AudioResult::Accepted(); }
	TerraEngine::Audio::AudioResult Shutdown() override { return TerraEngine::Audio::AudioResult::Accepted(); }
};
static_assert(TerraEngine::Audio::kAudioProviderContractVersion == 1U);
int main() { ExternalAudioProvider provider; return provider.Shutdown().Succeeded() ? 0 : 1; }
'@ | Set-Content $source -Encoding utf8
	if($IsWindows -or $env:OS -eq 'Windows_NT') {
		if(!(Get-Command cl.exe -ErrorAction SilentlyContinue)){throw 'cl.exe is required; run from a Visual Studio developer environment.'}
		$resolvedRoot = (Resolve-Path $IncludeRoot).Path
		if(!$LibraryPath) { $LibraryPath = Join-Path (Split-Path (Split-Path $resolvedRoot -Parent) -Parent) 'TerraEngine.lib' }
		if(!(Test-Path $LibraryPath)) { throw "TerraEngine import library was not found: $LibraryPath" }
		if(!$RuntimeDirectory) { $RuntimeDirectory = Join-Path (Split-Path $resolvedRoot -Parent) 'Engine' }
		$consumer = Join-Path $temporary 'consumer.exe'
		& cl.exe /nologo /std:c++20 /EHsc $source "/I$resolvedRoot" "/I$(Join-Path $resolvedRoot 'TerraEngine')" $LibraryPath "/Fe$consumer"
	} else {
		$compiler = Get-Command c++ -ErrorAction Stop
		$resolvedRoot = (Resolve-Path $IncludeRoot).Path
		if(!$LibraryPath) { $LibraryPath = Join-Path (Split-Path (Split-Path $resolvedRoot -Parent) -Parent) 'libTerraEngine.so' }
		if(!(Test-Path $LibraryPath)) { throw "TerraEngine shared library was not found: $LibraryPath" }
		if(!$RuntimeDirectory) { $RuntimeDirectory = Split-Path $LibraryPath -Parent }
		$consumer = Join-Path $temporary 'consumer'
		& $compiler.Source -std=c++20 $source -I $resolvedRoot -I (Join-Path $resolvedRoot 'TerraEngine') $LibraryPath -o $consumer
	}
	if($LASTEXITCODE -ne 0){throw 'Installed external-provider consumer failed to compile and link.'}
	$originalPath = $env:PATH
	try {
		if($RuntimeDirectory) { $env:PATH = "$RuntimeDirectory$([IO.Path]::PathSeparator)$env:PATH" }
		& $consumer
	} finally {
		$env:PATH = $originalPath
	}
	if($LASTEXITCODE -ne 0){throw 'Installed external-provider consumer executable failed its runtime check.'}
} finally {
	Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Output 'Installed provider contract compiled, linked, ran, and passed private SDK leakage checks.'
