#include "TestHarness.h"

#include "Resource/ResourceManager.h"
#include "Resource/ResourceResidency.h"
#include "Resource/ResourceStreaming.h"

namespace TerraEngine::Tests {
namespace {
	TerraEngine::UUID MakeId(std::uint8_t value)
	{
		TerraEngine::UUID id{};
		id.fill(0);
		id[15] = value;
		return id;
	}
}

	void RegisterResourceStreamingResidencyIntegrationTests(TestRegistry& registry)
	{
		registry.Add("Resource.Streaming.Residency.OwnedAnimatorNeedsPersistUntilRelease", [](TestContext& context) {
			ResourceManager& resources = ResourceManager::Instance();
			const auto asset = GenerateUUID(), firstOwner = GenerateUUID(), secondOwner = GenerateUUID();
			resources.RegisterStreamableAsset(asset, AssetType::Skeleton, "Tests/Fixtures/owned-residency.teskeleton");
			resources.MarkStreamingNeed(asset, AssetType::Skeleton, Resource::StreamingNeedType::RuntimeExplicit, 1, "active animator dependency", 100, firstOwner);
			resources.MarkStreamingNeed(asset, AssetType::Skeleton, Resource::StreamingNeedType::RuntimeExplicit, 1, "active animator dependency", 100, secondOwner);
			resources.MarkStreamingNeed(asset, AssetType::Skeleton, Resource::StreamingNeedType::RuntimeExplicit, 1, "transient explicit request");
			const auto published = resources.GetStreamableAsset(asset);
			context.Expect(published && published->needSources.size() == 3, "Native publication must retain two animator owners and one transient request.");
			// Apply the existing pure expiry calculation to actual native records.
			// No sleep or process-wide test clock is needed to prove their lifetime.
			const double future = published->lastNeededSeconds + 86400.0;
			const auto retained = Resource::MergeStreamingNeeds(published->needSources, future);
			context.Expect(retained.size() == 2 && retained[0].objectId != EMPTY_UUID && retained[1].objectId != EMPTY_UUID,
				"Owned animator needs must survive transient expiry without periodic publication spam.");
			context.Expect(resources.ReleaseStreamingNeedsForObject(firstOwner, Resource::StreamingNeedType::RuntimeExplicit) == 1,
				"Explicit removal must release only the first animator owner.");
			const auto shared = resources.GetStreamableAsset(asset);
			const auto remaining = Resource::MergeStreamingNeeds(shared->needSources, future);
			context.Expect(remaining.size() == 1 && remaining[0].objectId == secondOwner,
				"One owner removal must not unprotect another animator sharing the asset.");
			context.Expect(resources.ReleaseStreamingNeedsForObject(secondOwner, Resource::StreamingNeedType::RuntimeExplicit) == 1,
				"Removing the final owner must release its persistent need.");
			const auto released = resources.GetStreamableAsset(asset);
			context.Expect(Resource::MergeStreamingNeeds(released->needSources, future).empty(),
				"The remaining transient request must still expire normally.");
		});
		registry.Add("Resource.Streaming.Residency.RetentionDefersCleanup", [](TestContext& context) {
			Resource::ResidencyPolicy residency = Resource::MakeDefaultResidencyPolicy();
			residency.cpu.idleSecondsThreshold = 10.0;
			residency.cpu.reloadGraceSeconds = 5.0;
			residency.gpu.idleSecondsThreshold = 10.0;
			residency.gpu.reloadGraceSeconds = 5.0;

			Resource::ResidentAsset asset;
			asset.assetId = MakeId(7u);
			asset.assetType = Resource::ResidencyAssetType::Mesh;
			asset.sourceKey = "Assets/mesh.temf";
			asset.state = Resource::ResidencyState::Idle;
			asset.lastUsedSeconds = 8.0;
			asset.reloadable = true;

			Resource::ResidencySnapshot snapshot;
			snapshot.nowSeconds = 10.0;
			snapshot.policy = residency;
			snapshot.residentAssets.push_back(asset);

			context.Expect(Resource::BuildReleaseCandidates(snapshot).empty(), "Recently used streamed resources should not churn during the grace window.");
		});

		registry.Add("Renderer.VisibleStreamingTouches.BatchPublicationMarksUniqueResourcesVisible", [](TestContext& context) {
			ResourceManager& resourceManager = ResourceManager::Instance();
			const TerraEngine::UUID meshId = MakeId(101u);
			const TerraEngine::UUID materialId = MakeId(102u);
			const TerraEngine::UUID textureId = MakeId(103u);
			resourceManager.RegisterStreamableAsset(meshId, AssetType::Mesh, "Tests/Fixtures/visible-reuse-mesh.temf");
			resourceManager.RegisterStreamableAsset(materialId, AssetType::Material, "Tests/Fixtures/visible-reuse-material.tmat");
			resourceManager.RegisterStreamableAsset(textureId, AssetType::Texture, "Tests/Fixtures/visible-reuse-texture.png");

			TerraEngine::Renderer::VisibleStreamingTouchSet touchSet;
			touchSet.viewType = RenderViewType::Game;
			touchSet.outputId = 2u;
			touchSet.sourceCommandCount = 12u;
			touchSet.duplicateEliminationCount = 21u;
			touchSet.meshes.push_back({ meshId, ::VisibleStreamingResourceKind::Mesh, 100 });
			touchSet.materials.push_back({ materialId, ::VisibleStreamingResourceKind::Material, 100 });
			touchSet.textures.push_back({ textureId, 12.5, 90 });
			touchSet.valid = true;

			resourceManager.PublishVisibleStreamingTouches(touchSet, 77u, "visible reuse test");

			const std::optional<Resource::StreamableAssetRecord> meshRecord = resourceManager.GetStreamableAsset(meshId);
			const std::optional<Resource::StreamableAssetRecord> materialRecord = resourceManager.GetStreamableAsset(materialId);
			const std::optional<Resource::StreamableAssetRecord> textureRecord = resourceManager.GetStreamableAsset(textureId);

			context.Expect(meshRecord.has_value(), "Batch publication should keep the mesh tracked as streamable.");
			context.Expect(materialRecord.has_value(), "Batch publication should keep the material tracked as streamable.");
			context.Expect(textureRecord.has_value(), "Batch publication should keep the texture tracked as streamable.");
			context.Expect(meshRecord->lastVisibleSeconds > 0.0, "Visible mesh touch should refresh visible retention.");
			context.Expect(materialRecord->lastVisibleSeconds > 0.0, "Visible material touch should refresh visible retention.");
			context.Expect(textureRecord->lastVisibleSeconds > 0.0, "Visible texture touch should refresh visible retention.");
			context.Expect(textureRecord->textureClosestViewDistance <= 12.5, "Texture touch should preserve the nearest cached view distance.");
		});
	}
}
