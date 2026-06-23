#include "TestHarness.h"

#include "Resource/ResourceResidency.h"

namespace TerraEngine::Tests {
namespace {
	Resource::ResidentAsset MakeAsset(std::uint8_t idByte, double lastUsedSeconds)
	{
		Resource::ResidentAsset asset;
		asset.assetId.fill(0);
		asset.assetId[15] = idByte;
		asset.assetType = Resource::ResidencyAssetType::Texture;
		asset.sourceKey = "Assets/Test/Texture" + std::to_string(idByte) + ".tetf";
		asset.state = Resource::ResidencyState::Idle;
		asset.lastUsedSeconds = lastUsedSeconds;
		asset.estimatedCpuBytes = 128u;
		asset.estimatedGpuBytes = 384u;
		asset.reloadable = true;
		return asset;
	}
}

	void RegisterResourceResidencyPolicyTests(TestRegistry& registry)
	{
		registry.Add("Resource.Residency.Policy.SanitizesZeroIntervals", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.cleanupIntervalSeconds = 0.0;

			policy = Resource::SanitizeResidencyPolicy(policy);

			context.Expect(policy.cleanupIntervalSeconds > 0.0, "Cleanup intervals must never sanitize to zero because elapsed-time checks need a positive interval.");
		});

		registry.Add("Resource.Residency.Policy.IdleThresholdCreatesCandidates", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.cpu.idleSecondsThreshold = 1000.0;
			policy.cpu.reloadGraceSeconds = 0.0;
			policy.gpu.idleSecondsThreshold = 10.0;
			policy.gpu.reloadGraceSeconds = 0.0;
			const std::vector<Resource::ResidentAsset> assets = { MakeAsset(1u, 1.0), MakeAsset(2u, 19.0) };

			Resource::ResidencySnapshot snapshot;
			snapshot.frameId = 20u;
			snapshot.nowSeconds = 20.0;
			snapshot.policy = policy;
			snapshot.residentAssets = assets;
			const std::vector<Resource::ReleaseCandidate> candidates = Resource::BuildReleaseCandidates(snapshot);

			context.ExpectEqual(std::size_t{1u}, candidates.size(), "Only the asset older than the configured idle threshold should be released.");
			context.Expect(candidates.front().releaseReason == Resource::ResidencyReleaseReason::IdleExpiry, "Idle cleanup should explain that the candidate expired by age.");
			context.Expect(candidates.front().releaseDomain == Resource::ResidencyReleaseDomain::Gpu, "GPU can expire before RAM so warm CPU payloads stay available for fast reloads.");
		});
	}
}
