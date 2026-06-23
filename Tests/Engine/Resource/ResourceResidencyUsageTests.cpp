#include "TestHarness.h"

#include "Resource/ResourceResidency.h"

namespace TerraEngine::Tests {
namespace {
	Resource::ResidentAsset MakeUsageAsset(std::uint8_t idByte, Resource::ResidencyPinReason pinReason)
	{
		Resource::ResidentAsset asset;
		asset.assetId.fill(0);
		asset.assetId[15] = idByte;
		asset.assetType = Resource::ResidencyAssetType::Mesh;
		asset.sourceKey = "Assets/Test/Mesh" + std::to_string(idByte) + ".temf";
		asset.state = pinReason == Resource::ResidencyPinReason::None ? Resource::ResidencyState::Idle : Resource::ResidencyState::Pinned;
		asset.pinReason = pinReason;
		asset.lastUsedFrame = 0u;
		asset.lastUsedSeconds = 0.0;
		asset.estimatedCpuBytes = 256u;
		asset.estimatedGpuBytes = 768u;
		asset.reloadable = true;
		return asset;
	}
}

	void RegisterResourceResidencyUsageTests(TestRegistry& registry)
	{
		registry.Add("Resource.Residency.Usage.PinsBlockRelease", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.cpu.idleSecondsThreshold = 1000.0;
			policy.cpu.reloadGraceSeconds = 0.0;
			policy.gpu.idleSecondsThreshold = 1.0;
			policy.gpu.reloadGraceSeconds = 0.0;
			const std::vector<Resource::ResidentAsset> assets = {
				MakeUsageAsset(1u, Resource::ResidencyPinReason::ActiveScene),
				MakeUsageAsset(2u, Resource::ResidencyPinReason::EditorSelection),
				MakeUsageAsset(3u, Resource::ResidencyPinReason::None)
			};

			Resource::ResidencySnapshot snapshot;
			snapshot.frameId = 100u;
			snapshot.nowSeconds = 100.0;
			snapshot.policy = policy;
			snapshot.residentAssets = assets;
			const std::vector<Resource::ReleaseCandidate> candidates = Resource::BuildReleaseCandidates(snapshot);

			context.ExpectEqual(std::size_t{1u}, candidates.size(), "Pinned active/editor assets must be protected while idle unpinned assets remain releasable.");
			context.Expect(candidates.front().assetId[15] == 3u, "The only releasable candidate should be the unpinned mesh.");
			context.Expect(candidates.front().releaseDomain == Resource::ResidencyReleaseDomain::Gpu, "The first unpinned release should target GPU memory while RAM remains warm.");
		});

		registry.Add("Resource.Residency.Usage.ReloadGraceBlocksRecentAssets", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.cpu.idleSecondsThreshold = 1.0;
			policy.cpu.reloadGraceSeconds = 10.0;
			policy.gpu.idleSecondsThreshold = 1.0;
			policy.gpu.reloadGraceSeconds = 10.0;
			const Resource::ResidentAsset asset = MakeUsageAsset(4u, Resource::ResidencyPinReason::None);

			const Resource::ResidencyBlockedReason blocker = Resource::EvaluateReleaseBlocker(asset, policy, 5.0);

			context.Expect(blocker == Resource::ResidencyBlockedReason::RecentlyUsed, "Recently touched resources need a grace window so rapid unload/reload churn is avoided.");
		});

		registry.Add("Resource.Residency.Usage.ShaderCollectionsStayResident", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.cpu.idleSecondsThreshold = 1.0;
			policy.cpu.reloadGraceSeconds = 0.0;
			policy.gpu.idleSecondsThreshold = 1.0;
			policy.gpu.reloadGraceSeconds = 0.0;
			Resource::ResidentAsset shaderCollection = MakeUsageAsset(5u, Resource::ResidencyPinReason::None);
			shaderCollection.assetType = Resource::ResidencyAssetType::ShaderCollection;
			shaderCollection.sourceKey = "Engine/Shaders/default.tspf";
			shaderCollection.reloadable = false;
			shaderCollection.state = Resource::ResidencyState::Idle;

			Resource::ResidencySnapshot snapshot;
			snapshot.frameId = 100u;
			snapshot.nowSeconds = 100.0;
			snapshot.policy = policy;
			snapshot.residentAssets = { shaderCollection };
			const std::vector<Resource::ReleaseCandidate> candidates = Resource::BuildReleaseCandidates(snapshot);

			context.Expect(candidates.empty(), "Shader collections must stay resident until pipeline residency tracks their exact lifetime.");
		});
	}
}
