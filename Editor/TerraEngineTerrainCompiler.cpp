// TerraEngineTerrainCompiler.cpp -- Small headless terrain import validator.
//
// The editor uses the same compiler library. This command-line entry point is
// intentionally bounded: it decodes an input and reports the canonical sample
// dimensions, leaving asset publication to the editor creation service.

#include "AssetCompilers/TerrainCompiler.h"
#include "TerraEngine-Editor/Terrain/TerrainMaterialCompiler.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
	bool ReadRawPlane(const std::filesystem::path& path, std::size_t sampleCount,
		std::vector<std::uint8_t>& outBytes)
	{
		if(path == "-") return true;
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if(!input || static_cast<std::size_t>(input.tellg()) != sampleCount) return false;
		outBytes.resize(sampleCount);
		input.seekg(0, std::ios::beg);
		return static_cast<bool>(input.read(reinterpret_cast<char*>(outBytes.data()),
			static_cast<std::streamsize>(outBytes.size())));
	}
}

int main(int argc, char** argv)
{
	TerraEngine::AssetPipeline::TerrainImportRequest request{};
	// The headless validator creates an ephemeral identity because it does not
	// register a project asset; editor creation supplies the durable .meta UUID.
	request.assetId = TerraEngine::GenerateUUID();
	if (argc < 2)
	{
		std::cerr << "usage: TerraEngineTerrainCompiler --flat | --png16 <path> | --raw16 <path> <width> <height> | --orm <ao.raw|-> <roughness.raw|-> <metallic.raw|-> <width> <height> <generated-dir>\n";
		return 2;
	}
	const std::string_view mode(argv[1]);
	if(mode == "--orm" && argc >= 8)
	{
		const std::uint32_t width = static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 10));
		const std::uint32_t height = static_cast<std::uint32_t>(std::strtoul(argv[6], nullptr, 10));
		const std::size_t sampleCount = static_cast<std::size_t>(width) * height;
		std::array<std::vector<std::uint8_t>, 3u> planes;
		if(width == 0u || height == 0u
			|| !ReadRawPlane(argv[2], sampleCount, planes[0])
			|| !ReadRawPlane(argv[3], sampleCount, planes[1])
			|| !ReadRawPlane(argv[4], sampleCount, planes[2]))
		{
			std::cerr << "terrain ORM sources must be raw 8-bit planes matching width x height, or '-' for neutral fallback\n";
			return 2;
		}
		auto makePlane = [](const std::vector<std::uint8_t>& bytes, std::uint8_t neutral) {
			TerraEngine::Editor::Terrain::TerrainMaterialSourcePlane plane;
			plane.samples = bytes;
			plane.neutralValue = neutral;
			if(!bytes.empty())
			{
				plane.sourceIdentity = TerraEngine::Terrain::HashTerrainBytes(bytes);
				plane.sourceGeneration = 1u;
			}
			return plane;
		};
		TerraEngine::Editor::Terrain::TerrainOrmCompileRequest orm;
		orm.width = width;
		orm.height = height;
		orm.occlusion = makePlane(planes[0], 255u);
		orm.roughness = makePlane(planes[1], 255u);
		orm.metallic = makePlane(planes[2], 0u);
		orm.generatedContentDirectory = argv[7];
		const auto compiled = TerraEngine::Editor::Terrain::CompileTerrainOrmArtifact(orm);
		if(!compiled.succeeded)
		{
			std::cerr << compiled.message << '\n';
			return 1;
		}
		std::cout << "terrain_orm hash=" << TerraEngine::Terrain::ToHexString(compiled.derivationKey)
			<< " artifact=" << compiled.artifactPath.string()
			<< " dependencies=" << compiled.sourceDependencies.size() << '\n';
		return 0;
	}
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
