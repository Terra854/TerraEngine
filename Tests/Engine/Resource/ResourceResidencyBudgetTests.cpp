#include "TestHarness.h"

#include "Resource/ResourceResidency.h"

namespace TerraEngine::Tests {
namespace {
	Resource::ResidentAsset MakeBudgetAsset(std::uint8_t idByte, double lastUsedSeconds, std::uint64_t bytes)
	{
		Resource::ResidentAsset asset;
		asset.assetId.fill(0);
		asset.assetId[15] = idByte;
		asset.assetType = Resource::ResidencyAssetType::Texture;
		asset.sourceKey = "Assets/Test/Budget" + std::to_string(idByte) + ".tetf";
		asset.state = Resource::ResidencyState::Idle;
		asset.lastUsedSeconds = lastUsedSeconds;
		asset.estimatedCpuBytes = bytes / 2u;
		asset.estimatedGpuBytes = bytes / 2u;
		asset.reloadable = true;
		return asset;
	}
}

	void RegisterResourceResidencyBudgetTests(TestRegistry& registry)
	{
		registry.Add("Resource.Residency.Budget.OldestUnusedSelectedFirst", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.idleSecondsThreshold = 1000.0;
			policy.reloadGraceSeconds = 0.0;
			policy.memoryBudgetBytes = 512u;
			const std::vector<Resource::ResidentAsset> assets = {
				MakeBudgetAsset(1u, 90u, 400u),
				MakeBudgetAsset(2u, 10u, 400u),
				MakeBudgetAsset(3u, 70u, 400u)
			};

			Resource::ResidencySnapshot snapshot;
			snapshot.frameId = 100u;
			snapshot.nowSeconds = 100.0;
			snapshot.policy = policy;
			snapshot.residentAssets = assets;
			const std::vector<Resource::ReleaseCandidate> candidates = Resource::BuildReleaseCandidates(snapshot);

			context.Expect(!candidates.empty(), "Over-budget residency should produce memory-pressure candidates even before idle expiry.");
			context.Expect(candidates.front().assetId[15] == 2u, "Budget cleanup should release the oldest unused asset first for deterministic behavior.");
		});

		registry.Add("Resource.Residency.Budget.AllPinnedReportsNoCandidates", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.memoryBudgetBytes = 1u;
			std::vector<Resource::ResidentAsset> assets = { MakeBudgetAsset(5u, 1u, 1024u) };
			assets.front().pinReason = Resource::ResidencyPinReason::FallbackResource;
			assets.front().state = Resource::ResidencyState::Pinned;

			Resource::ResidencySnapshot snapshot;
			snapshot.frameId = 100u;
			snapshot.nowSeconds = 100.0;
			snapshot.policy = policy;
			snapshot.residentAssets = assets;
			const std::vector<Resource::ReleaseCandidate> candidates = Resource::BuildReleaseCandidates(snapshot);

			context.Expect(candidates.empty(), "Pinned fallback/default resources must survive budget pressure.");
		});
	}
}
