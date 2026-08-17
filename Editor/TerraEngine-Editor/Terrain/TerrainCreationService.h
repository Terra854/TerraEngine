// TerrainCreationService.h -- Create/import and parent-asset publication service.

#pragma once

#include "Editor/AssetCompilers/TerrainCompiler.h"

#include <functional>
#include <string>

namespace TerraEngine::Editor::Terrain
{
	struct TerrainCreationResult
	{
		bool succeeded = false;
		bool cancelled = false;
		std::filesystem::path assetPath;
		TerraEngine::AssetPipeline::TerrainHeightfield heightfield{};
		std::string message;
	};

	class TerrainCreationService
	{
	public:
		using ParentAssetRegistration = std::function<bool(const std::filesystem::path& assetPath, std::string& outError)>;
		explicit TerrainCreationService(ParentAssetRegistration registerParent = {}) : registerParentAsset(std::move(registerParent)) {}
		// The registration callback runs only after a complete temporary descriptor
		// has been atomically renamed. Failed validation/cancellation leaves no asset.
		[[nodiscard]] TerrainCreationResult CreateOrImport(
			const TerraEngine::AssetPipeline::TerrainImportRequest& request,
			const std::filesystem::path& destination,
			TerraEngine::AssetPipeline::TerrainCompileProgressCallback progress = {}) const;
	private:
		ParentAssetRegistration registerParentAsset;
	};
}
