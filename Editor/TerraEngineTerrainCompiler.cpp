// TerraEngineTerrainCompiler.cpp -- Small headless terrain import validator.
//
// The editor uses the same compiler library. This command-line entry point is
// intentionally bounded: it decodes an input and reports the canonical sample
// dimensions, leaving asset publication to the editor creation service.

#include "AssetCompilers/TerrainCompiler.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
	TerraEngine::AssetPipeline::TerrainImportRequest request{};
	// The headless validator creates an ephemeral identity because it does not
	// register a project asset; editor creation supplies the durable .meta UUID.
	request.assetId = TerraEngine::GenerateUUID();
	if (argc < 2)
	{
		std::cerr << "usage: TerraEngineTerrainCompiler --flat | --png16 <path> | --raw16 <path> <width> <height>\n";
		return 2;
	}
	const std::string_view mode(argv[1]);
	if (mode == "--flat")
	{
		request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Flat;
	}
	else if (mode == "--png16" && argc >= 3)
	{
		request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Png16;
		request.sourcePath = argv[2];
	}
	else if (mode == "--raw16" && argc >= 5)
	{
		request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Raw16LittleEndian;
		request.sourcePath = argv[2];
		request.rawWidth = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10));
		request.rawHeight = static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10));
	}
	else
	{
		std::cerr << "invalid terrain compiler arguments\n";
		return 2;
	}
	const auto result = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
	if (!result.succeeded)
	{
		std::cerr << result.message << '\n';
		return result.cancelled ? 3 : 1;
	}
	std::cout << "terrain_heightfield width=" << result.heightfield.width
		<< " height=" << result.heightfield.height
		<< " samples=" << result.heightfield.heightsMm.size() << '\n';
	return 0;
}
