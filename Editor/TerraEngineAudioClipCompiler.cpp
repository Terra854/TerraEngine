// TerraEngineAudioClipCompiler.cpp - small deterministic command-line TEAF cooker.

#include "AssetCompilers/AudioClipCompiler.h"

#include <iostream>

int main(int argc, char** argv)
{
	// Keep the standalone tool deliberately small: project/editor code owns meta
	// GUID allocation, while automation supplies the stable 32-hex UUID here.
	if(argc != 4) {
		std::cerr << "Usage: TerraEngineAudioClipCompiler <source.wav|source.ogg> <output.teaudio> <asset-uuid>\n";
		return 2;
	}
	const auto assetId = TerraEngine::ParseUUID(argv[3]);
	if(!assetId) {
		std::cerr << "Invalid asset UUID.\n";
		return 2;
	}
	const TerraEngine::AssetPipeline::AudioClipCompileReport report =
		TerraEngine::AssetPipeline::CompileAudioClip({argv[1], argv[2], *assetId});
	if(!report.Succeeded()) {
		std::cerr << TerraEngine::AssetPipeline::ToString(report.status) << ": " << report.diagnostic << '\n';
		return 1;
	}
	std::cout << "Compiled " << report.sourceBytes << " bytes, " << report.metadata.frameCount << " frames.\n";
	return 0;
}
