// TerrainCreationService.h -- Create/import and parent-asset publication service.

#pragma once

#include "Editor/AssetCompilers/TerrainCompiler.h"
#include "Terrain/TerrainAssetStore.h"

#include <functional>
#include <map>
#include <future>
#include <memory>
#include <atomic>
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
		TerraEngine::Terrain::TerrainSampleBounds authoredBounds{};
		std::shared_ptr<TerraEngine::Terrain::TerrainAssetStore> store;
		std::map<TerraEngine::Terrain::TerrainAreaCoordinate,
			TerraEngine::Terrain::TerrainAreaDescriptor> areaSnapshots;
		bool compatibilityHeightfieldComplete = false;
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
		// Large terrain creation uses the shared task pool. All filesystem and asset
		// registration work remains inside the same atomic synchronous implementation.
		[[nodiscard]] std::future<TerrainCreationResult> CreateOrImportAsync(
			TerraEngine::AssetPipeline::TerrainImportRequest request,
			std::filesystem::path destination,
			std::shared_ptr<std::atomic_bool> cancellation,
			TerraEngine::AssetPipeline::TerrainCompileProgressCallback progress = {}) const;
		// Scene reload returns the validated root/store and also assembles a bounded
		// compatibility heightfield for existing editor callers. No origin-area
		// shortcut may override the root's authored extent.
		[[nodiscard]] TerrainOpenResult OpenExisting(const std::filesystem::path& source) const;
		[[nodiscard]] static bool SynchronizeCompatibilityDescriptor(
			const std::filesystem::path& descriptorPath,
			const TerraEngine::Terrain::TerrainAssetRoot& committedRoot,
			std::string& outError);
	private:
		ParentAssetRegistration registerParentAsset;
	};

	// Editor UI surfaces use this configured service so a completed terrain is
	// registered through AssetDatabase and ResourceManager before it is exposed.
	// Keeping the callback here prevents each window from implementing a subtly
	// different publication path for the same durable terrain bundle.
	[[nodiscard]] TerrainCreationService CreateProjectTerrainCreationService();
}
