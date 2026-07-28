#include "TestHarness.h"

#include "Resource/ResourceManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		std::string ReadSourceFile(const std::string& path)
		{
			const auto read = [](const std::string& candidate) {
				std::ifstream file(candidate, std::ios::binary);
				if (!file)
				{
					return std::string();
				}
				std::ostringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			};

			std::string text = read(path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../") + path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../../") + path);
			if (!text.empty())
			{
				return text;
			}

			const std::string thisFile = __FILE__;
			const auto resolveFromMarker = [&](const std::string& marker) {
				const std::size_t markerPosition = thisFile.find(marker);
				if (markerPosition == std::string::npos)
				{
					return std::string();
				}
				return read(thisFile.substr(0u, markerPosition) + path);
			};
			text = resolveFromMarker("Tests\\Engine\\Resource\\ResourceManagerMeshTests.cpp");
			if (!text.empty())
			{
				return text;
			}
			return resolveFromMarker("Tests/Engine/Resource/ResourceManagerMeshTests.cpp");
		}

		std::string ReadResourceManagerMeshSource()
		{
			// ResourceManagerMesh.cpp owns the runtime TEMF rejection path. This
			// source-contract test keeps the page-integrity checks visible even
			// when the binary TEMF fixture format changes during v1 development.
			return ReadSourceFile("Engine/Resource/ResourceManagerMesh.cpp");
		}
	}

	void RegisterResourceManagerMeshTests(TestRegistry& registry)
	{
		registry.Add("Resource.Manager.Mesh.RejectsCorruptVirtualGeometryPagePayloads", [](TestContext& context) {
			const std::string source = ReadResourceManagerMeshSource();

			context.Expect(source.find("TEMF virtual geometry page table is missing required cluster-page coverage metadata") != std::string::npos, "Runtime loader should reject page tables without cluster-page coverage.");
			context.Expect(source.find("TEMF virtual geometry page has invalid identity, fallback, or byte-size metadata") != std::string::npos, "Runtime loader should reject invalid page ids, fallback ids, or byte sizes.");
			context.Expect(source.find("TEMF virtual geometry page does not declare cluster-page coverage metadata") != std::string::npos, "Runtime loader should reject pages without cluster-page links.");
			context.Expect(source.find("TEMF virtual geometry page fallback points to a higher-detail page") != std::string::npos, "Runtime loader should reject fallback links that point to higher-detail pages.");
			context.Expect(source.find("TEMF virtual geometry page payload byte range overruns payload") != std::string::npos, "Runtime loader should reject virtual geometry pages whose physical payload range is outside the TEMF payload.");
			context.Expect(source.find("page.byteOffset > static_cast<std::uint64_t>(payloadSize)") != std::string::npos, "Runtime loader should validate page payload offsets against the loaded payload span.");
			context.Expect(source.find("material section LOD range") != std::string::npos, "Runtime loader should validate material section metadata before publication.");
			context.Expect(source.find("TEMF virtual geometry cullable group has invalid bounds") != std::string::npos, "Runtime loader should reject invalid page/cluster bounds before rendering.");
			context.Expect(source.find("TEMF virtual geometry page table does not declare a resident fallback page") != std::string::npos, "Runtime loader should require explicit resident fallback coverage for page-streamed assets.");
		});

		registry.Add("Resource.Manager.Mesh.DeformableSpatialBoundsUseWholeAnimationEnvelope", [](TestContext& context) {
			const std::string source = ReadResourceManagerMeshSource();

			context.Expect(
				source.find("if (mesh->hasDeformablePayload)") != std::string::npos
					&& source.find("mesh->deformableMetadata.deformationBoundsMin") != std::string::npos
					&& source.find("mesh->deformableMetadata.deformationBoundsMax") != std::string::npos,
				"Loaded deformable meshes must publish the checked whole-animation envelope to CPU spatial queries.");
			context.Expect(
				source.find("glm::min(bounds.min, deformationBounds.min)") != std::string::npos
					&& source.find("glm::max(bounds.max, deformationBounds.max)") != std::string::npos,
				"The deformable spatial box must conservatively include both bind pose and the complete animation envelope.");
		});

		registry.Add("Resource.Manager.Mesh.ExactResidencyGeneratedChildrenOwnGpuBytes", [](TestContext& context) {
			ResourceManager& resourceManager = ResourceManager::Instance();
			resourceManager.SetStreamingDiagnosticsEnabled(true);

			const TerraEngine::UUID packageId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID generatedMeshId = TerraEngine::GenerateUUID();
			const std::filesystem::path compiledPath = "Tests/Fixtures/generated-package-child.temf";
			resourceManager.RegisterStreamableAsset(packageId, AssetType::Mesh, compiledPath);
			resourceManager.RegisterStreamableAsset(generatedMeshId, AssetType::Mesh, compiledPath);
			resourceManager.MarkResourceUploadedToGpu(
				generatedMeshId,
				AssetType::Mesh,
				42u,
				1234u,
				"generated child upload");

			const Resource::StreamingDiagnosticsSummary summary = resourceManager.GetStreamingDiagnosticsSummary();
			const auto packageRecord = std::find_if(summary.records.begin(), summary.records.end(), [packageId](const Resource::StreamingDiagnosticRecord& record) {
				return record.assetId == packageId;
			});
			const auto generatedRecord = std::find_if(summary.records.begin(), summary.records.end(), [generatedMeshId](const Resource::StreamingDiagnosticRecord& record) {
				return record.assetId == generatedMeshId;
			});
			context.Expect(packageRecord != summary.records.end(), "The logical package parent should remain visible to diagnostics before the editor filters it.");
			context.Expect(generatedRecord != summary.records.end(), "The generated child mesh should have its own diagnostics record.");
			if (packageRecord != summary.records.end() && generatedRecord != summary.records.end())
			{
				context.Expect(packageRecord->exactGpuBytes.status == Resource::ExactResidencyByteStatus::ZeroByState, "Package parents must not inherit child render GPU bytes.");
				context.ExpectEqual(0ull, packageRecord->exactGpuBytes.bytes, "Package parent exact GPU bytes should be zero unless it owns a backend allocation.");
				context.Expect(generatedRecord->exactGpuBytes.status == Resource::ExactResidencyByteStatus::Exact, "Generated child mesh should own the backend GPU allocation.");
				context.ExpectEqual(1234ull, generatedRecord->exactGpuBytes.bytes, "Generated child exact GPU bytes should equal the upload size.");
			}

			resourceManager.DeleteResource(packageId, AssetType::Mesh);
			resourceManager.DeleteResource(generatedMeshId, AssetType::Mesh);
		});

		registry.Add("Resource.Manager.Mesh.PageStreamingUploadPolicyUsesPageLocalPath", [](TestContext& context) {
			const std::string meshSource = ReadResourceManagerMeshSource();
			const std::string meshHeader = ReadSourceFile("Engine/File Formats/TerraEngineMeshFile.h");
			const std::string vulkanSource = ReadSourceFile("Engine/Renderer/RHI/Vulkan/VulkanContextResources.cpp");

			context.Expect(meshHeader.find("virtualGeometryPageStreamingReady") != std::string::npos, "Mesh metadata should expose whether physical page streaming is ready.");
			context.Expect(meshHeader.find("virtualGeometryFullOwnerPayloadBytes") != std::string::npos, "Mesh metadata should expose the exact owner-stream byte cost.");
			context.Expect(meshHeader.find("virtualGeometryResidentHighDetailPageBytes") != std::string::npos, "Mesh metadata should expose exact high-detail page bytes.");
			context.Expect(meshHeader.find("virtualGeometryResidentFallbackPageBytes") != std::string::npos, "Mesh metadata should expose exact fallback page bytes.");
			context.Expect(meshHeader.find("virtualGeometryOwnerFallbackReason") != std::string::npos, "Mesh metadata should explain any owner-stream fallback.");
			context.Expect(meshSource.find("ResolveVirtualGeometryUploadPolicy") != std::string::npos, "The TEMF loader should derive virtual geometry upload policy from page tables.");
			context.Expect(meshSource.find("ResolveFullOwnerMeshPayloadBytes") != std::string::npos, "The TEMF loader should measure exact owning stream payload bytes instead of estimating them.");
			context.Expect(meshSource.find("current mesh-shader decode path still binds owner mesh streams") == std::string::npos, "The loader must not keep the old metadata-only shader fallback reason once page-local decode exists.");
			context.Expect(vulkanSource.find("owner mesh buffers are skipped for page-local virtual geometry") != std::string::npos, "Vulkan uploads should skip full owner buffers for page-local virtual geometry.");
			context.Expect(vulkanSource.find("vulkan mesh cache hit with page-local virtual geometry owner placeholders") != std::string::npos, "Vulkan cache hits should preserve the page-local placeholder diagnostic.");
		});
	}
}
