#include "TestHarness.h"

#include "Resource/ResourceResidency.h"

namespace TerraEngine::Tests {
namespace {
	Resource::ResidentAsset MakeDiagnosticAsset()
	{
		Resource::ResidentAsset asset;
		asset.assetId.fill(7u);
		asset.assetType = Resource::ResidencyAssetType::Material;
		asset.sourceKey = "Assets/Test/Material.tmat";
		asset.state = Resource::ResidencyState::Idle;
		asset.lastUsedFrame = 0u;
		asset.lastUsedSeconds = 0.0;
		asset.estimatedCpuBytes = 64u;
		asset.estimatedGpuBytes = 128u;
		asset.reloadable = true;
		return asset;
	}
}

	void RegisterResourceResidencyDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Resource.Residency.Diagnostics.SummaryAlwaysCountsResidentBytes", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.diagnosticsEnabled = false;
			const std::vector<Resource::ResidentAsset> assets = { MakeDiagnosticAsset() };

			Resource::ResidencySnapshot snapshot;
			snapshot.frameId = 20u;
			snapshot.nowSeconds = 20.0;
			snapshot.policy = policy;
			snapshot.residentAssets = assets;
			const Resource::ResidencyDiagnosticsSummary summary = Resource::BuildResidencyDiagnostics(snapshot, {}, policy.diagnosticsEnabled);

			context.ExpectEqual(std::uint64_t{192u}, summary.estimatedResidentBytes, "Lightweight diagnostics should still expose aggregate memory without detailed per-asset cost.");
			context.Expect(summary.candidates.empty(), "Detailed candidate lists must stay disabled unless diagnostics are explicitly enabled.");
		});

		registry.Add("Resource.Residency.Diagnostics.CollectsDetailsOnlyWhenEnabled", [](TestContext& context) {
			Resource::ResidencyPolicy policy = Resource::MakeDefaultResidencyPolicy();
			policy.diagnosticsEnabled = true;
			policy.cpu.idleSecondsThreshold = 1.0;
			policy.cpu.reloadGraceSeconds = 0.0;
			policy.gpu.idleSecondsThreshold = 1.0;
			policy.gpu.reloadGraceSeconds = 0.0;
			const std::vector<Resource::ResidentAsset> assets = { MakeDiagnosticAsset() };

			Resource::ResidencySnapshot snapshot;
			snapshot.frameId = 20u;
			snapshot.nowSeconds = 20.0;
			snapshot.policy = policy;
			snapshot.residentAssets = assets;
			const Resource::ResidencyDiagnosticsSummary summary = Resource::BuildResidencyDiagnostics(snapshot, {}, policy.diagnosticsEnabled);

			context.Expect(!summary.candidates.empty(), "Open diagnostics should include candidate explanations for developer inspection.");
		});
	}
}
