#include "TestHarness.h"
#include "Renderer/VulkanSourceTestUtils.h"

#include "File Formats/TerraEngineMaterialFile.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cstring>
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
			// ResourceManagerMesh.cpp owns reconstruction checks after the
			// canonical file validator accepts the complete TEMFPRD1 envelope.
			return ReadSourceFile("Engine/Resource/ResourceManagerMesh.cpp");
		}

		std::string ReadVirtualGeometryCodecSource()
		{
			// VirtualGeometryCodec.cpp owns whole-file directory, block-range,
			// dependency, and integrity validation before reconstruction starts.
			return ReadSourceFile("Engine/Resource/VirtualGeometryCodec.cpp");
		}
	}

	void RegisterResourceManagerMeshTests(TestRegistry& registry)
	{
		registry.Add("Resource.Manager.Mesh.RejectsCorruptVirtualGeometryPagePayloads", [](TestContext& context) {
			const std::string source = ReadResourceManagerMeshSource();
			const std::string codecSource = ReadVirtualGeometryCodecSource();

			context.Expect(source.find("TEMF virtual geometry page table is missing required cluster-page coverage metadata") != std::string::npos, "Runtime loader should reject page tables without cluster-page coverage.");
			context.Expect(source.find("TEMF virtual geometry page has invalid identity, fallback, or byte-size metadata") != std::string::npos, "Runtime loader should reject invalid page ids, fallback ids, or byte sizes.");
			context.Expect(source.find("TEMF virtual geometry page does not declare cluster-page coverage metadata") != std::string::npos, "Runtime loader should reject pages without cluster-page links.");
			context.Expect(source.find("TEMF virtual geometry page fallback points to a higher-detail page") != std::string::npos, "Runtime loader should reject fallback links that point to higher-detail pages.");
			context.Expect(codecSource.find("TEMFPRD1 block metadata is malformed or exceeds a declared limit.") != std::string::npos, "The canonical validator should reject a block whose physical byte range escapes the TEMF payload.");
			context.Expect(codecSource.find("TEMFPRD1 page blocks must occupy one contiguous payload range.") != std::string::npos, "The canonical validator should prove every page is backed by one checked block range.");
			context.Expect(source.find("material section LOD range") != std::string::npos, "Runtime loader should validate material section metadata before publication.");
			context.Expect(source.find("TEMF virtual geometry cullable group has invalid bounds") != std::string::npos, "Runtime loader should reject invalid page/cluster bounds before rendering.");
			context.Expect(source.find("TEMF virtual geometry page table does not declare a resident fallback page") != std::string::npos, "Runtime loader should require explicit resident fallback coverage for page-streamed assets.");
			context.Expect(source.find("ParseVirtualGeometryPayload") == std::string::npos, "The runtime loader must not retain the retired inline virtual-geometry reader.");
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
			const std::string vulkanSource = VulkanSourceContracts::ReadResourceSources();

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

		registry.Add("Resource.Manager.RendererReloadsPublishOnlyCompletedPayloads", [](TestContext& context) {
			ResourceManager& resourceManager = ResourceManager::Instance();
			const std::uint64_t failedReloadGeneration =
				resourceManager.GetRendererResourcePublicationGeneration();
			const TerraEngine::UUID unknownId = TerraEngine::GenerateUUID();
			resourceManager.ReloadMesh(unknownId);
			resourceManager.ReloadMaterial(unknownId);
			resourceManager.ReloadTexture(unknownId).get();
			context.ExpectEqual(failedReloadGeneration,
				resourceManager.GetRendererResourcePublicationGeneration(),
				"Reload requests without a completed cache payload must not publish a new renderer resource generation.");

			const std::string header = ReadSourceFile("Engine/Resource/ResourceManager.h");
			const std::string mesh = ReadSourceFile("Engine/Resource/ResourceManagerMesh.cpp");
			const std::string material = ReadSourceFile("Engine/Resource/ResourceManagerMaterial.cpp");
			const std::string texture = ReadSourceFile("Engine/Resource/ResourceManagerTexture.cpp");
			const std::string publication =
				"m_RendererResourcePublicationGeneration.fetch_add(1U, std::memory_order_release);";
			// Generated resource registration now publishes the same generation before
			// the reload functions in these files. Restrict source-order assertions to
			// each reload implementation so an earlier valid publication is not mistaken
			// for an invalid failed-reload publication.
			const std::string meshReload = mesh.substr(mesh.find("ResourceManager::ReloadMesh("));
			const std::string materialReload = material.substr(material.find("ResourceManager::ReloadMaterial("));
			const std::string textureReload = texture.substr(texture.find("ResourceManager::ReloadTexture("));

			context.Expect(header.find("GetRendererResourcePublicationGeneration")
					!= std::string::npos
				&& header.find("std::atomic_uint64_t m_RendererResourcePublicationGeneration{1U}")
					!= std::string::npos,
				"ResourceManager must expose a monotonic renderer-only publication generation.");
			context.Expect(meshReload.find("meshCache.resources[id] = std::move(mesh);")
					< meshReload.find(publication)
				&& materialReload.find("materialCache.resources[id] = std::move(material);")
					< materialReload.find(publication)
				&& textureReload.find("textureCache.resources[id] = std::move(texture);")
					< textureReload.find(publication),
				"Mesh, material, and texture reloads must advance renderer publication only after replacing a complete cache payload.");
			context.Expect(meshReload.find("if (mesh)") < meshReload.find(publication)
				&& materialReload.find("if (material)") < materialReload.find(publication)
				&& textureReload.find("if (texture)") < textureReload.find(publication),
				"Reload failures must not advance the renderer publication generation.");
		});

		registry.Add("Resource.Manager.InitialRendererLoadInvalidatesWarmRenderCaches", [](TestContext& context) {
			ResourceManager& resourceManager = ResourceManager::Instance();
			context.Expect(IsRendererResourceAssetType(AssetType::Mesh)
					&& IsRendererResourceAssetType(AssetType::Material)
					&& IsRendererResourceAssetType(AssetType::Texture)
					&& IsRendererResourceAssetType(AssetType::ShaderCollection)
					&& !IsRendererResourceAssetType(AssetType::AnimationClip),
				"Only renderer-owned asset caches should publish this invalidation generation.");

			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			const std::filesystem::path materialPath =
				std::filesystem::temp_directory_path()
				/ ("terra-live-material-" + TerraEngine::ToHexString(materialId) + ".tmat");
			MaterialHeader header{};
			std::memcpy(header.magic, "TMAT", sizeof(header.magic));
			header.version = kMaterialCurrentVersion;
			header.headerSize = static_cast<std::uint16_t>(sizeof(MaterialHeader));
			header.textureArrayOffset = header.headerSize;
			header.floatParameterOffset = header.headerSize;
			{
				// The smallest valid TMAT is sufficient to exercise the same lazy first
				// load used when an Inspector assigns a previously unreferenced material.
				std::ofstream output(materialPath, std::ios::binary | std::ios::trunc);
				output.write(reinterpret_cast<const char*>(&header), sizeof(header));
				context.Expect(static_cast<bool>(output),
					"The first-load regression fixture must be written completely.");
			}

			resourceManager.RegisterAssetPath(materialId, materialPath, AssetType::Material);
			const std::uint64_t beforeLoad =
				resourceManager.GetRendererResourcePublicationGeneration();
			context.Expect(resourceManager.GetMaterial(materialId) != nullptr,
				"A registered material should load through its stable Inspector UUID.");
			context.Expect(
				resourceManager.GetRendererResourcePublicationGeneration() > beforeLoad,
				"A successful first renderer-resource load must invalidate warm render caches.");

			const TerraEngine::UUID missingMaterialId = TerraEngine::GenerateUUID();
			const std::filesystem::path missingMaterialPath =
				materialPath.parent_path()
				/ ("terra-missing-material-"
					+ TerraEngine::ToHexString(missingMaterialId) + ".tmat");
			resourceManager.RegisterAssetPath(
				missingMaterialId, missingMaterialPath, AssetType::Material);
			const std::uint64_t beforeFailedLoad =
				resourceManager.GetRendererResourcePublicationGeneration();
			context.Expect(resourceManager.GetMaterial(missingMaterialId) == nullptr,
				"A missing first-load candidate must not create a material payload.");
			context.ExpectEqual(
				beforeFailedLoad,
				resourceManager.GetRendererResourcePublicationGeneration(),
				"A failed first load must retain the prior renderer generation.");

			resourceManager.DeleteResource(materialId, AssetType::Material);
			resourceManager.DeleteResource(missingMaterialId, AssetType::Material);
			std::error_code removeError;
			std::filesystem::remove(materialPath, removeError);
		});
	}
}
