#include "TestHarness.h"

#include "Resource/ResourceManager.h"
#include "Resource/ResourceStreaming.h"

#include <algorithm>
#include <filesystem>

namespace TerraEngine::Tests {
	void RegisterResourceStreamingDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Resource.Streaming.Diagnostics.StringsDescribeStates", [](TestContext& context) {
			context.ExpectEqual(std::string_view("deferred"), Resource::ToString(Resource::StreamablePayloadState::Deferred), "Deferred state should be readable in diagnostics.");
			context.ExpectEqual(std::string_view("failed"), Resource::ToString(Resource::StreamablePayloadState::Failed), "Failed state should be readable in diagnostics.");
			context.ExpectEqual(std::string_view("prefab-editing"), Resource::ToString(Resource::StreamingProtectionReason::PrefabEditing), "Prefab protection should be readable in diagnostics.");
		});

		registry.Add("Resource.Streaming.Diagnostics.ClosedByDefault", [](TestContext& context) {
			const Resource::StreamingPolicy policy = Resource::MakeDefaultStreamingPolicy();

			context.Expect(!policy.diagnosticsEnabled, "Detailed streaming diagnostics should be disabled until a diagnostics surface opens.");
		});

		registry.Add("Resource.Streaming.Diagnostics.LiveGpuUploadBytesOverrideStaleSnapshot", [](TestContext& context) {
			ResourceManager& resourceManager = ResourceManager::Instance();
			resourceManager.SetStreamingDiagnosticsEnabled(true);

			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			resourceManager.RegisterStreamableAsset(meshId, AssetType::Mesh, "Tests/Fixtures/live-upload-mesh.temf");
			const Resource::StreamingDiagnosticsSummary beforeUpload = resourceManager.GetStreamingDiagnosticsSummary();
			const auto beforeRecord = std::find_if(beforeUpload.records.begin(), beforeUpload.records.end(), [meshId](const Resource::StreamingDiagnosticRecord& record) {
				return record.assetId == meshId;
			});
			context.Expect(beforeRecord != beforeUpload.records.end(), "Registered mesh should appear in live streaming diagnostics before upload.");
			if (beforeRecord != beforeUpload.records.end())
			{
				context.ExpectEqual(0ull, beforeRecord->estimatedGpuBytes, "Unuploaded mesh should start with zero diagnostic GPU bytes.");
			}

			resourceManager.MarkResourceUploadedToGpu(
				meshId,
				AssetType::Mesh,
				42u,
				4096u,
				"test backend upload");
			const Resource::StreamingDiagnosticsSummary afterUpload = resourceManager.GetStreamingDiagnosticsSummary();
			const auto afterRecord = std::find_if(afterUpload.records.begin(), afterUpload.records.end(), [meshId](const Resource::StreamingDiagnosticRecord& record) {
				return record.assetId == meshId;
			});
			context.Expect(afterRecord != afterUpload.records.end(), "Uploaded mesh should remain visible in live streaming diagnostics.");
			if (afterRecord != afterUpload.records.end())
			{
				context.ExpectEqual(4096ull, afterRecord->estimatedGpuBytes, "Diagnostics should report backend-published GPU bytes without waiting for cleanup.");
				context.Expect(afterRecord->payloadState == Resource::StreamablePayloadState::VramResident, "Uploaded mesh should report VRAM residency immediately.");
				context.Expect(afterRecord->sourceKey.find("live-upload-mesh.temf") != std::string::npos, "Backend upload diagnostics should preserve the registered streamable source key.");
			}

			resourceManager.DeleteResource(meshId, AssetType::Mesh);
		});

		registry.Add("Resource.Streaming.Diagnostics.GeneratedMeshUploadsKeepCompiledSourceAlias", [](TestContext& context) {
			ResourceManager& resourceManager = ResourceManager::Instance();
			resourceManager.SetStreamingDiagnosticsEnabled(true);

			const TerraEngine::UUID packageId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID generatedMeshId = TerraEngine::GenerateUUID();
			const std::filesystem::path compiledPath = "Tests/Fixtures/shared-package-mesh.temf";
			resourceManager.RegisterStreamableAsset(packageId, AssetType::Mesh, compiledPath);
			resourceManager.RegisterStreamableAsset(generatedMeshId, AssetType::Mesh, compiledPath);

			resourceManager.MarkResourceUploadedToGpu(
				generatedMeshId,
				AssetType::Mesh,
				77u,
				8192u,
				"generated mesh backend upload");

			const Resource::StreamingDiagnosticsSummary summary = resourceManager.GetStreamingDiagnosticsSummary();
			const auto packageRecord = std::find_if(summary.records.begin(), summary.records.end(), [packageId](const Resource::StreamingDiagnosticRecord& record) {
				return record.assetId == packageId;
			});
			const auto generatedRecord = std::find_if(summary.records.begin(), summary.records.end(), [generatedMeshId](const Resource::StreamingDiagnosticRecord& record) {
				return record.assetId == generatedMeshId;
			});
			context.Expect(packageRecord != summary.records.end(), "Package mesh wrapper should remain registered for diagnostics.");
			context.Expect(generatedRecord != summary.records.end(), "Generated mesh child should remain registered for diagnostics.");
			if (packageRecord != summary.records.end() && generatedRecord != summary.records.end())
			{
				context.ExpectEqual(packageRecord->sourceKey, generatedRecord->sourceKey, "Package and generated mesh diagnostics should keep the shared compiled TEMF source key.");
				context.ExpectEqual(0ull, packageRecord->estimatedGpuBytes, "Package wrapper should not invent direct GPU bytes.");
				context.ExpectEqual(8192ull, generatedRecord->estimatedGpuBytes, "Generated child should publish the render backend GPU bytes.");
			}

			resourceManager.DeleteResource(packageId, AssetType::Mesh);
			resourceManager.DeleteResource(generatedMeshId, AssetType::Mesh);
		});
	}
}
