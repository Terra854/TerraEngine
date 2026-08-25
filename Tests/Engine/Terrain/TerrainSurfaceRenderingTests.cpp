// TerrainSurfaceRenderingTests.cpp -- control residency and coupled derived publication coverage.

#include "TestHarness.h"

#include "Renderer/Terrain/TerrainSurfaceCache.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include "Components/TerrainComponent.h"
#include "Terrain/TerrainDerivedSystem.h"
#include "Terrain/TerrainMaterial.h"
#include "Terrain/TerrainLayerStack.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "Resource/ResourceManager.h"

#include <cstring>
#include <memory>

namespace TerraEngine::Tests
{
	void RegisterTerrainSurfaceRenderingTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Surface.ComponentPersistsSanitizedPbrInputs", [](TestContext& context) {
			TerrainComponent terrain;
			terrain.baseColor = {-1.0F, 0.4F, 2.0F};
			terrain.metallic = 1.5F;
			terrain.roughness = -0.25F;
			const auto settings = terrain.MaterialSettings();
			context.Expect(TerrainComponent::kSchemaVersion == 2U
				&& settings.baseColor == glm::vec3(0.0F, 0.4F, 1.0F)
				&& settings.metallic == 1.0F && settings.roughness == 0.0F,
				"Terrain scene material inputs must persist under schema v2 and clamp to valid metallic-roughness ranges.");
		});

		registry.Add("Terrain.Surface.GeneratedProxyCopiesFirstLayerTextureAndMapping", [](TestContext& context) {
			Material source{};
			TextureBinding binding{};
			binding.semantic = TextureSemantic::Base;
			binding.textureUuid = TerraEngine::GenerateUUID();
			binding.uvScale[0] = 2.0F;
			binding.uvScale[1] = 3.0F;
			source.storage.resize(sizeof(binding));
			std::memcpy(source.storage.data(), &binding, sizeof(binding));
			source.textures = std::span<const TextureBinding>(
				reinterpret_cast<const TextureBinding*>(source.storage.data()), 1u);
			TerraEngine::Terrain::TerrainLayerMapping mapping{};
			mapping.scaleX = 4.0;
			mapping.scaleZ = 5.0;
			mapping.offsetX = 0.25;
			mapping.offsetZ = 0.5;
			auto generated = TerraEngine::Terrain::CreateTerrainMaterial(
				ResourceManager::Instance(), {}, &source, &mapping);
			context.Expect(generated && generated->textures.size() == 1u
				&& generated->textures[0].textureUuid == binding.textureUuid
				&& generated->textures[0].uvScale[0] == 8.0F
				&& generated->textures[0].uvScale[1] == 15.0F
				&& generated->textures[0].uvOffset[0] == 0.25F
				&& generated->textures[0].uvOffset[1] == 0.5F,
				"The generated terrain proxy must own a copied layer texture binding and apply its authored mapping.");
		});

		registry.Add("Terrain.Surface.RuntimeBindingPublishesAtomically", [](TestContext& context) {
			auto& runtimeRegistry = TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeRegistry::Instance();
			runtimeRegistry.Clear();
			TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeBinding incomplete{};
			incomplete.materialId = TerraEngine::GenerateUUID();
			context.Expect(!runtimeRegistry.Publish(incomplete) && !runtimeRegistry.Resolve(incomplete.materialId),
				"An incomplete terrain control binding must be rejected without leaving partial renderer state.");

			TerraEngine::Renderer::Terrain::TerrainMaterialRuntimeBinding complete{};
			complete.materialId = TerraEngine::GenerateUUID();
			complete.weightTextureId = TerraEngine::GenerateUUID();
			complete.layerIdTextureId = TerraEngine::GenerateUUID();
			complete.layerCount = 1u;
			complete.layers[0].stableLayerId = 7u;
			context.Expect(runtimeRegistry.Publish(complete),
				"A complete terrain control binding must publish as one protected value.");
			const auto resolved = runtimeRegistry.Resolve(complete.materialId);
			context.Expect(resolved && resolved->publicationGeneration > 0u
				&& resolved->layers[0].stableLayerId == 7u,
				"The renderer must resolve one immutable generation containing controls and stable layer metadata together.");
			runtimeRegistry.Remove(complete.materialId);
			context.Expect(!runtimeRegistry.Resolve(complete.materialId),
				"Releasing a terrain proxy must also remove its renderer binding.");
		});

		registry.Add("Terrain.Surface.CacheKeepsProtectedControlAndRejectsStalePublication", [](TestContext& context) {
			const auto asset = TerraEngine::GenerateUUID();
			TerraEngine::Renderer::Terrain::TerrainSurfaceCacheRequest pinned{};
			pinned.key = {asset, {0, 0}, {1u, 2u}, 4u}; pinned.byteCount = 80u; pinned.priority = 10u; pinned.protectedFromEviction = true;
			TerraEngine::Renderer::Terrain::TerrainSurfaceCacheRequest ordinary{};
			ordinary.key = {asset, {1, 0}, {3u, 4u}, 4u}; ordinary.byteCount = 80u;
			const auto plan = TerraEngine::Renderer::Terrain::PrepareTerrainSurfaceCachePlan(4u, {}, {pinned, ordinary}, 100u);
			std::vector<TerraEngine::Renderer::Terrain::TerrainSurfacePageHandle> visible;
			std::uint64_t generation = 0u;
			context.Expect(plan.candidate.size() == 1u && plan.candidate.front().key.area == pinned.key.area,
				"Control cache pressure must retain a protected page and evict ordinary material detail.");
			context.Expect(!TerraEngine::Renderer::Terrain::PublishTerrainSurfaceCachePlan(visible, generation, plan, 5u)
				&& TerraEngine::Renderer::Terrain::PublishTerrainSurfaceCachePlan(visible, generation, plan, 4u),
				"A stale control-page candidate must not replace last-known-good visible surface state.");
			TerraEngine::Renderer::Terrain::TerrainSurfaceCacheRequest replacement = pinned;
			replacement.key.sourceHash = {9u, 9u};
			const auto lkgPlan = TerraEngine::Renderer::Terrain::PrepareTerrainSurfaceCachePlan(5u, visible, {replacement}, 80u);
			context.Expect(lkgPlan.candidate.size() == 1u && lkgPlan.candidate.front().lastKnownGood
				&& lkgPlan.candidate.front().key.sourceHash == pinned.key.sourceHash,
				"An over-budget replacement must retain the prior exact control page as last-known-good coverage.");
		});

		registry.Add("Terrain.Surface.DerivedBatchRequiresGeometryHolesAndControlsTogether", [](TestContext& context) {
			auto root = std::make_shared<Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 2u; root->rootContentHash = {8u, 9u}; root->authoredBounds = {0, 0, 1, 1};
			Terrain::TerrainAssetStore store(root);
			Terrain::TerrainDerivedSurfaceBatch batch{};
			batch.source = {root->assetUuid, root->rootContentHash, root->publicationGeneration, true, nullptr};
			batch.derivedGeneration = 1u; batch.geometryIdentity = {1u, 1u}; batch.holeIdentity = {2u, 2u}; batch.surfaceControlIdentity = {3u, 3u};
			batch.geometryComplete = batch.holesComplete = batch.surfaceControlsComplete = true;
			const auto accepted = Terrain::ValidateTerrainDerivedSurfaceBatch(batch, store);
			batch.surfaceControlsComplete = false;
			const auto rejected = Terrain::ValidateTerrainDerivedSurfaceBatch(batch, store);
			context.Expect(accepted.Succeeded() && !rejected.Succeeded(),
				"Geometry, hole coverage, and material controls must pass one generation-checked publication barrier.");
		});
	}
}
