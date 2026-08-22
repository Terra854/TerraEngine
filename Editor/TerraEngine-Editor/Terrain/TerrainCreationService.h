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

	struct TerrainOpenResult
	{
		bool succeeded = false;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		float sampleSpacingMetres = 1.0F;
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
		// Scene reload uses the same checked area codec as terrain creation. Only
		// the first authored area is materialized because the current direct proxy
		// is explicitly a one-area authoring surface.
		[[nodiscard]] TerrainOpenResult OpenExisting(const std::filesystem::path& source) const;
	private:
		ParentAssetRegistration registerParentAsset;
	};

	// Editor UI surfaces use this configured service so a completed terrain is
	// registered through AssetDatabase and ResourceManager before it is exposed.
	// Keeping the callback here prevents each window from implementing a subtly
	// different publication path for the same durable terrain bundle.
	[[nodiscard]] TerrainCreationService CreateProjectTerrainCreationService();
}
