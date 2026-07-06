#include "TestHarness.h"

#include "Resource/ResourceManager.h"
#include "Resource/ResourceResidency.h"

#include <string>

namespace TerraEngine::Tests {
namespace {
	TerraEngine::UUID MakeResidencyTestId(std::uint8_t value)
	{
		TerraEngine::UUID id{};
		id.fill(0);
		id[15] = value;
		return id;
	}

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

		registry.Add("Resource.Residency.Diagnostics.ExactCpuGpuBytesCountOnlyLiveOwnership", [](TestContext& context) {
			const TerraEngine::UUID assetId = MakeResidencyTestId(81u);
			const TerraEngine::UUID otherAssetId = MakeResidencyTestId(82u);
			std::vector<Resource::CpuPayloadOwnershipRecord> cpuRecords;
			Resource::CpuPayloadOwnershipRecord loadedCpu;
			loadedCpu.ownerAssetId = assetId;
			loadedCpu.byteSize = 128u;
			loadedCpu.state = Resource::CpuPayloadOwnershipState::Loaded;
			cpuRecords.push_back(loadedCpu);
			Resource::CpuPayloadOwnershipRecord retainedCpu = loadedCpu;
			retainedCpu.byteSize = 64u;
			retainedCpu.state = Resource::CpuPayloadOwnershipState::Retained;
			cpuRecords.push_back(retainedCpu);
			Resource::CpuPayloadOwnershipRecord pendingCpu = loadedCpu;
			pendingCpu.byteSize = 512u;
			pendingCpu.state = Resource::CpuPayloadOwnershipState::PendingRelease;
			cpuRecords.push_back(pendingCpu);
			Resource::CpuPayloadOwnershipRecord releasedCpu = loadedCpu;
			releasedCpu.byteSize = 1024u;
			releasedCpu.state = Resource::CpuPayloadOwnershipState::Released;
			cpuRecords.push_back(releasedCpu);
			Resource::CpuPayloadOwnershipRecord otherCpu = loadedCpu;
			otherCpu.ownerAssetId = otherAssetId;
			otherCpu.byteSize = 2048u;
			cpuRecords.push_back(otherCpu);

			const Resource::ExactResidencyByteValue cpuBytes = Resource::AggregateCpuPayloadBytes(cpuRecords, assetId);
			context.Expect(cpuBytes.status == Resource::ExactResidencyByteStatus::Exact, "Live CPU ownership records should produce an exact value.");
			context.ExpectEqual(192ull, cpuBytes.bytes, "CPU exact bytes should include only loaded and retained records for this asset.");

			std::vector<Resource::GpuAllocationOwnershipRecord> gpuRecords;
			Resource::GpuAllocationOwnershipRecord publishedGpu;
			publishedGpu.ownerAssetId = assetId;
			publishedGpu.byteSize = 256u;
			publishedGpu.state = Resource::GpuAllocationOwnershipState::Published;
			gpuRecords.push_back(publishedGpu);
			Resource::GpuAllocationOwnershipRecord inFlightGpu = publishedGpu;
			inFlightGpu.byteSize = 128u;
			inFlightGpu.state = Resource::GpuAllocationOwnershipState::InFlight;
			gpuRecords.push_back(inFlightGpu);
			Resource::GpuAllocationOwnershipRecord retiringGpu = publishedGpu;
			retiringGpu.byteSize = 64u;
			retiringGpu.state = Resource::GpuAllocationOwnershipState::PendingRetirement;
			gpuRecords.push_back(retiringGpu);
			Resource::GpuAllocationOwnershipRecord retiredGpu = publishedGpu;
			retiredGpu.byteSize = 4096u;
			retiredGpu.state = Resource::GpuAllocationOwnershipState::Retired;
			gpuRecords.push_back(retiredGpu);

			const Resource::ExactResidencyByteValue gpuBytes = Resource::AggregateGpuAllocationBytes(gpuRecords, assetId);
			context.Expect(gpuBytes.status == Resource::ExactResidencyByteStatus::Exact, "Live GPU ownership records should produce an exact value.");
			context.ExpectEqual(448ull, gpuBytes.bytes, "GPU exact bytes should include published, in-flight, and safely-retiring allocations only.");
		});

		registry.Add("Resource.Residency.Diagnostics.ExactByteStatesExposeZeroAndUnavailable", [](TestContext& context) {
			const TerraEngine::UUID assetId = MakeResidencyTestId(83u);
			Resource::CpuPayloadOwnershipRecord releasedCpu;
			releasedCpu.ownerAssetId = assetId;
			releasedCpu.byteSize = 1024u;
			releasedCpu.state = Resource::CpuPayloadOwnershipState::Released;

			const Resource::ExactResidencyByteValue exactZero = Resource::AggregateCpuPayloadBytes({ releasedCpu }, assetId);
			context.Expect(exactZero.status == Resource::ExactResidencyByteStatus::ZeroByState, "Released CPU records should produce an exact zero instead of an unavailable value.");
			context.ExpectEqual(0ull, exactZero.bytes, "Zero-by-state CPU bytes should be numeric zero.");

			const Resource::ExactResidencyByteValue unavailable = Resource::AggregateGpuAllocationBytes({}, assetId);
			context.Expect(unavailable.status == Resource::ExactResidencyByteStatus::Unavailable, "Missing GPU ownership records should remain unavailable rather than guessed as zero.");
			context.Expect(!unavailable.unavailableReason.empty(), "Unavailable exact bytes should preserve an explanation.");

			Resource::ResidentAsset asset;
			asset.assetId = assetId;
			asset.exactCpuBytes = exactZero;
			asset.exactGpuBytes = unavailable;
			const std::vector<Resource::ExactResidencyUnavailableDiagnostic> diagnostics =
				Resource::BuildExactResidencyUnavailableDiagnostics({ asset });
			context.ExpectEqual(std::size_t{1u}, diagnostics.size(), "Only unavailable byte domains should create diagnostics.");
			context.ExpectEqual(std::string("gpu"), diagnostics.front().domain, "Unavailable diagnostics should identify the missing memory domain.");
		});

		registry.Add("Resource.Residency.Diagnostics.LiveGpuUploadBytesOverrideStaleSnapshot", [](TestContext& context) {
			ResourceManager& resourceManager = ResourceManager::Instance();
			resourceManager.SetResidencyDiagnosticsEnabled(true);

			const Resource::ResidencyDiagnosticsSummary beforeUpload = resourceManager.GetResidencyDiagnosticsSummary();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			resourceManager.MarkResourceUploadedToGpu(
				meshId,
				AssetType::Mesh,
				84u,
				8192u,
				"test backend upload");

			const Resource::ResidencyDiagnosticsSummary afterUpload = resourceManager.GetResidencyDiagnosticsSummary();
			context.Expect(
				afterUpload.estimatedResidentBytes >= beforeUpload.estimatedResidentBytes + 8192u,
				"Residency diagnostics should include backend-published mesh GPU bytes without waiting for cleanup.");

			resourceManager.DeleteResource(meshId, AssetType::Mesh);
		});
	}
}
