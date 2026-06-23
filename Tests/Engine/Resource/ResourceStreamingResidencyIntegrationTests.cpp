#include "TestHarness.h"

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
		registry.Add("Resource.Streaming.Residency.RetentionDefersCleanup", [](TestContext& context) {
			Resource::ResidencyPolicy residency = Resource::MakeDefaultResidencyPolicy();
			residency.idleSecondsThreshold = 10.0;
			residency.reloadGraceSeconds = 5.0;

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
	}
}
