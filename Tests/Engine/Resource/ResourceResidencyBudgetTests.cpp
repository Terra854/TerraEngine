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
			policy.cpu.idleSecondsThreshold = 1000.0;
			policy.cpu.reloadGraceSeconds = 0.0;
			policy.cpu.memoryBudgetBytes = 0u;
			policy.gpu.idleSecondsThreshold = 1000.0;
			policy.gpu.reloadGraceSeconds = 0.0;
			policy.gpu.memoryBudgetBytes = 512u;
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
			context.Expect(candidates.front().releaseDomain == Resource::ResidencyReleaseDomain::Gpu, "GPU budget pressure should not force RAM eviction.");
		});

		registry.Add("Resource.Residency.Budget.AllPinnedReportsNoCandidates", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.cpu.memoryBudgetBytes = 1u;
			policy.gpu.memoryBudgetBytes = 1u;
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

		registry.Add("Resource.Residency.Budget.PriorityAndTierInfluenceEvictionOrder", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.cpu.memoryBudgetBytes = 0u;
			policy.gpu.idleSecondsThreshold = 1000.0;
			policy.gpu.reloadGraceSeconds = 0.0;
			policy.gpu.memoryBudgetBytes = 256u;
			Resource::ResidentAsset background = MakeBudgetAsset(1u, 10u, 400u);
			background.priorityClass = Resource::StreamingPriorityClass::Background;
			background.residencyTier = Resource::StreamingResidencyTier::Streamed;
			Resource::ResidentAsset critical = MakeBudgetAsset(2u, 1u, 400u);
			critical.priorityClass = Resource::StreamingPriorityClass::Critical;
			critical.residencyTier = Resource::StreamingResidencyTier::Persistent;

			Resource::ResidencySnapshot snapshot;
			snapshot.frameId = 100u;
			snapshot.nowSeconds = 100.0;
			snapshot.policy = policy;
			snapshot.residentAssets = { critical, background };
			const std::vector<Resource::ReleaseCandidate> candidates = Resource::BuildReleaseCandidates(snapshot);

			context.Expect(!candidates.empty(), "Budget pressure should still produce candidates when priority metadata is present.");
			context.Expect(candidates.front().assetId[15] == 1u, "Background streamed assets should be reclaimed before critical persistent assets.");
			context.Expect(candidates.front().priorityClass == Resource::StreamingPriorityClass::Background, "Candidate diagnostics should preserve the release priority class.");
			context.Expect(candidates.front().residencyTier == Resource::StreamingResidencyTier::Streamed, "Candidate diagnostics should preserve the residency tier.");
		});
	}
}
