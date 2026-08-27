// TerrainRuntimeBootstrap.cpp -- protected export-scene terrain request capture.

#include "Runtime/ContentMount/TerrainRuntimeBootstrap.h"

#include "Components/TerrainComponent.h"
#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "Resource/ResourceManager.h"

#include <filesystem>
#include <unordered_set>

namespace TerraEngine::Runtime
{
	std::vector<MountedTerrainBootstrapRequest> CaptureMountedTerrainBootstrapRequests()
	{
		ResourceManager& resources = ResourceManager::Instance();
		std::unordered_set<TerraEngine::UUID> scheduledAssets;
		std::vector<MountedTerrainBootstrapRequest> requests;
		ObjectManager::instance().forEachObjectWithComponents<TerrainComponent>(
			[&](Object, TerrainComponent& terrain)
			{
				if(!terrain.visible || terrain.terrainAssetId == TerraEngine::EMPTY_UUID
					|| !scheduledAssets.emplace(terrain.terrainAssetId).second) return;
				const std::filesystem::path registeredPath = resources.GetRegisteredAssetPath(
					terrain.terrainAssetId, AssetType::Terrain);
				if(registeredPath.empty())
				{
					TERRAENGINE_LOG(LogLevel::Error,
						"Export terrain asset is absent from game.json: "
						<< TerraEngine::ToHexString(terrain.terrainAssetId));
					return;
				}

				// ResourceManager stores preferred paths as normalized absolute keys.
				// ContentMount accepts archive-relative logical paths, so the protected
				// capture removes the runtime asset root before dispatching worker IO.
				const std::filesystem::path mountedPath = registeredPath.is_absolute()
					? registeredPath.lexically_relative(resources.GetAssetRoot()) : registeredPath;
				const std::string mountedPathText = mountedPath.generic_string();
				if(mountedPath.empty() || mountedPathText.starts_with(".."))
				{
					TERRAENGINE_LOG(LogLevel::Error,
						"Export terrain path is outside the mounted content root: " << registeredPath);
					return;
				}
				requests.push_back({terrain.terrainAssetId, mountedPathText, terrain.MaterialSettings()});
			});
		return requests;
	}
}
