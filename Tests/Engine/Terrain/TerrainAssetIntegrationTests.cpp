// TerrainAssetIntegrationTests.cpp — foundation component and runtime path checks.

#include "../TestHarness.h"

#include "Components/TerrainComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/Reflection.h"
#include "Resource/ResourceManager.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ContentMount/TerrainContentResolver.h"
#include "Terrain/TerrainAssetStore.h"

#include <filesystem>

namespace TerraEngine::Tests
{
	void RegisterTerrainAssetIntegrationTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Foundation.ComponentReflectionAndPlacement", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			TerrainComponent::registerType();
			const TerraEngine::Reflection::Type type = TerraEngine::Reflection::ResolveType<TerrainComponent>();
			context.Expect(TerraEngine::Reflection::IsValid(type), "TerrainComponent must publish reflection metadata before scene loading.");
			for(const char* property : { "schemaVersion", "terrainAssetId", "visible", "castsShadows" }) {
				context.Expect(TerraEngine::Reflection::IsValid(TerraEngine::Reflection::FindProperty(type, property)),
					"TerrainComponent durable fields must remain reflected scene properties.");
			}

			TerraEngine::Scene::WorldPose pose{};
			context.Expect(TerrainComponent::SupportsTranslationOnlyPlacement(pose), "Identity rotation and unit scale must permit a terrain translation.");
			pose.rotation.x = 0.1F;
			context.Expect(!TerrainComponent::SupportsTranslationOnlyPlacement(pose), "Terrain v1 must reject rotated placements.");
			pose.rotation = { 1.0F, 0.0F, 0.0F, 0.0F };
			pose.scale.x = 2.0F;
			context.Expect(!TerrainComponent::SupportsTranslationOnlyPlacement(pose), "Terrain v1 must reject scaled placements.");
		});

		registry.Add("Terrain.Foundation.AssetPathAndMountedReadFailClosed", [](TestContext& context) {
			const TerraEngine::UUID terrainId = TerraEngine::GenerateUUID();
			const std::filesystem::path path = "Terrain/Foundation.teterrain";
			ResourceManager::Instance().RegisterAssetPath(terrainId, path, AssetType::Terrain);
			context.ExpectEqual(ResourceManager::Instance().GetRegisteredAssetPath(terrainId, AssetType::Terrain).filename(),
				path.filename(), "Terrain roots must retain their registered runtime path without entering a generic cache.");

			ContentMount unmounted;
			const TerrainContentReadResult read = TerrainContentResolver::ReadRoot(unmounted, "Terrain/Foundation.teterrain");
			context.Expect(read.status == TerrainContentStatus::NotMounted,
				"A packaged terrain root must fail closed when no TEPAK archive is mounted.");
			ResourceManager::Instance().DeleteResource(terrainId, AssetType::Terrain);
		});

		registry.Add("Terrain.Creation.AuthoredBoundsEnumerateExplicitAndImplicitAreas", [](TestContext& context) {
			auto root = std::make_shared<Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID();
			root->publicationGeneration = 1u;
			root->rootContentHash = {1u, 2u};
			root->authoredBounds = {-1, -1, 513, 257};
			root->defaultLayerId = 1u;
			Terrain::TerrainAreaDescriptor explicitArea{};
			explicitArea.coordinate = {0, 0};
			explicitArea.editGeneration = 1u;
			explicitArea.authoredContentHash = {3u, 4u};
			Terrain::TerrainDirectoryShard shard{};
			shard.areaBounds = {-1, -1, 3, 2};
			shard.descriptors.emplace(explicitArea.coordinate, explicitArea);
			root->directoryShards.push_back(std::move(shard));
			Terrain::TerrainAssetStore store(root);
			const auto areas = Terrain::EnumerateTerrainAreas(root->authoredBounds, 32u);
			bool allResolvable = areas.has_value();
			if(areas) for(const auto coordinate : *areas)
				allResolvable = allResolvable && store.ResolveDescriptorOrImplicit(*root, coordinate).authoredContentHash.IsValid();
			context.Expect(areas && areas->size() == 12u && allResolvable,
				"Root-authored bounds must expose every signed explicit or implicit area, including partial edges.");
		});
	}
}
