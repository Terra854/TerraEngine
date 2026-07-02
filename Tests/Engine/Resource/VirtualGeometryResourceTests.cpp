#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace TerraEngine::Tests
{
namespace
{
	enum class AdaptiveFixtureCorruption
	{
		None,
		ClusterSelfParent,
		MissingFallbackCluster,
		InvalidMaterialSection
	};

	std::uint32_t AlignForTest(std::uint32_t value, std::uint32_t alignment)
	{
		return (value + alignment - 1u) & ~(alignment - 1u);
	}

	template <typename T>
	void WriteValue(std::vector<std::uint8_t>& bytes, std::uint32_t offset, const T& value)
	{
		const auto* source = reinterpret_cast<const std::uint8_t*>(&value);
		std::copy(source, source + sizeof(T), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
	}

	void WriteStaleVirtualGeometryTemf(const std::filesystem::path& path, bool advertiseStaleVirtualGeometry = true)
	{
		MeshHeader header{};
		header.magic[0] = 'T';
		header.magic[1] = 'E';
		header.magic[2] = 'M';
		header.magic[3] = 'F';
		header.version = kMeshCurrentVersion;
		header.headerSize = static_cast<std::uint32_t>(sizeof(MeshHeader));
		header.meshletVertexCount = 3u;
		header.meshletVertexRemapCount = 3u;
		header.meshletPrimitiveIndexCount = 3u;
		header.meshletCount = 1u;
		header.meshletSectionCount = 1u;
		header.meshletBoundsCount = 1u;
		header.meshletVertexRemapEncoding = static_cast<std::uint32_t>(MeshletRemapEncoding::Uint32);
		header.meshletFlags = kMeshletHeaderFlagHasBounds
			| (advertiseStaleVirtualGeometry ? kMeshletHeaderFlagHasVirtualGeometry : 0u);
		header.layout.strideBytes = kMeshletPackedVertexStrideBytes;
		header.layout.attributeCount = 4u;
		header.layout.attributes[0] = { static_cast<std::uint32_t>(AttributeSemantic::Position), static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm), 0u };
		header.layout.attributes[1] = { static_cast<std::uint32_t>(AttributeSemantic::Normal), static_cast<std::uint32_t>(AttributeFormat::Oct32), 4u };
		header.layout.attributes[2] = { static_cast<std::uint32_t>(AttributeSemantic::Tangent), static_cast<std::uint32_t>(AttributeFormat::Oct32), 8u };
		header.layout.attributes[3] = { static_cast<std::uint32_t>(AttributeSemantic::TexCoord0), static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm), 12u };
		header.quantization.positionScale[0] = 1.0f;
		header.quantization.positionScale[1] = 1.0f;
		header.quantization.positionScale[2] = 1.0f;
		header.quantization.uvScale[0] = 1.0f;
		header.quantization.uvScale[1] = 1.0f;

		std::uint32_t cursor = AlignForTest(header.headerSize, kMeshletVertexDataAlignmentBytes);
		header.meshletVertexDataOffset = cursor;
		cursor = AlignForTest(cursor + header.meshletVertexCount * header.layout.strideBytes, kMeshletIndexDataAlignmentBytes);
		header.meshletVertexRemapDataOffset = cursor;
		cursor = AlignForTest(cursor + header.meshletVertexRemapCount * sizeof(std::uint32_t), kMeshletIndexDataAlignmentBytes);
		header.meshletPrimitiveIndexDataOffset = cursor;
		cursor = AlignForTest(cursor + header.meshletPrimitiveIndexCount * sizeof(std::uint8_t), kMeshletIndexDataAlignmentBytes);
		header.meshletTableOffset = cursor;
		cursor = AlignForTest(cursor + sizeof(MeshletRecord), kMeshletIndexDataAlignmentBytes);
		header.meshletSectionOffset = cursor;
		cursor = AlignForTest(cursor + sizeof(MeshletSection), kMeshletIndexDataAlignmentBytes);
		header.meshletBoundsOffset = cursor;
		cursor = AlignForTest(cursor + sizeof(MeshletBounds), kMeshletIndexDataAlignmentBytes);

		std::vector<std::uint8_t> bytes(cursor, 0u);
		WriteValue(bytes, 0u, header);

		const std::uint32_t remap[3] = { 0u, 1u, 2u };
		std::copy(reinterpret_cast<const std::uint8_t*>(remap), reinterpret_cast<const std::uint8_t*>(remap) + sizeof(remap), bytes.begin() + header.meshletVertexRemapDataOffset);

		const std::uint8_t primitives[3] = { 0u, 1u, 2u };
		std::copy(primitives, primitives + sizeof(primitives), bytes.begin() + header.meshletPrimitiveIndexDataOffset);

		MeshletRecord meshlet{};
		meshlet.vertexOffset = 0u;
		meshlet.vertexCount = 3u;
		meshlet.primitiveIndexOffset = 0u;
		meshlet.primitiveIndexCount = 3u;
		WriteValue(bytes, header.meshletTableOffset, meshlet);

		MeshletSection section{};
		section.meshletOffset = 0u;
		section.meshletCount = 1u;
		WriteValue(bytes, header.meshletSectionOffset, section);

		MeshletBounds bounds{};
		bounds.radius = 1.0f;
		bounds.coneAxis[2] = 1.0f;
		bounds.coneCutoff = 2.0f;
		WriteValue(bytes, header.meshletBoundsOffset, bounds);

		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	void WriteAdaptiveVirtualGeometryTemf(const std::filesystem::path& path, AdaptiveFixtureCorruption corruption = AdaptiveFixtureCorruption::None)
	{
		MeshHeader header{};
		header.magic[0] = 'T';
		header.magic[1] = 'E';
		header.magic[2] = 'M';
		header.magic[3] = 'F';
		header.version = kMeshCurrentVersion;
		header.headerSize = static_cast<std::uint32_t>(sizeof(MeshHeader));
		header.meshletVertexCount = 3u;
		header.meshletVertexRemapCount = 3u;
		header.meshletPrimitiveIndexCount = 3u;
		header.meshletCount = 1u;
		header.meshletSectionCount = 1u;
		header.meshletBoundsCount = 1u;
		header.meshletVertexRemapEncoding = static_cast<std::uint32_t>(MeshletRemapEncoding::Uint32);
		header.meshletFlags = kMeshletHeaderFlagHasBounds | kMeshletHeaderFlagHasVirtualGeometry;
		header.virtualGeometryHeaderSize = static_cast<std::uint32_t>(sizeof(VirtualGeometryHeader));
		header.virtualGeometryPayloadGeneration = kVirtualGeometryPayloadGeneration;
		header.layout.strideBytes = kMeshletPackedVertexStrideBytes;
		header.layout.attributeCount = 4u;
		header.layout.attributes[0] = { static_cast<std::uint32_t>(AttributeSemantic::Position), static_cast<std::uint32_t>(AttributeFormat::R10G10B10A2Snorm), 0u };
		header.layout.attributes[1] = { static_cast<std::uint32_t>(AttributeSemantic::Normal), static_cast<std::uint32_t>(AttributeFormat::Oct32), 4u };
		header.layout.attributes[2] = { static_cast<std::uint32_t>(AttributeSemantic::Tangent), static_cast<std::uint32_t>(AttributeFormat::Oct32), 8u };
		header.layout.attributes[3] = { static_cast<std::uint32_t>(AttributeSemantic::TexCoord0), static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm), 12u };
		header.quantization.positionScale[0] = 1.0f;
		header.quantization.positionScale[1] = 1.0f;
		header.quantization.positionScale[2] = 1.0f;
		header.quantization.uvScale[0] = 1.0f;
		header.quantization.uvScale[1] = 1.0f;

		std::string strings;
		const auto appendString = [&](std::string_view text) {
			const std::uint32_t offset = static_cast<std::uint32_t>(strings.size());
			strings.append(text.data(), text.size());
			strings.push_back('\0');
			return offset;
		};
		const std::uint32_t sectionNameOffset = appendString("DefaultSection");
		const std::uint32_t skippedMessageOffset = appendString("target has too few triangles");
		const std::uint32_t validationMessageOffset = appendString("fallback coverage validated");

		VirtualGeometryDetailLevelRecord detail{};
		detail.lodIndex = 0u;
		detail.requestedTargetIndex = 0u;
		detail.targetStatus = static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Generated);
		detail.meshletOffset = 0u;
		detail.meshletCount = 1u;
		detail.vertexCount = 3u;
		detail.primitiveCount = 1u;
		detail.sectionRangeOffset = 0u;
		detail.sectionRangeCount = 1u;
		detail.pageRangeOffset = 0u;
		detail.pageRangeCount = 1u;
		detail.targetRatio = 1.0f;
		detail.boundsMin[0] = detail.boundsMin[1] = detail.boundsMin[2] = 0.0f;
		detail.boundsMax[0] = detail.boundsMax[1] = detail.boundsMax[2] = 1.0f;

		VirtualGeometryMaterialSectionRecord material{};
		material.sectionId = 0u;
		material.materialSlot = 0u;
		material.sourceTriangleCount = 1u;
		material.perLodRangeOffset = 0u;
		material.perLodRangeCount = 1u;
		material.sourceNameStringOffset = sectionNameOffset;
		material.sourceNameStringSize = static_cast<std::uint32_t>(std::string_view("DefaultSection").size());

		VirtualGeometrySectionRangeRecord section{};
		section.lodIndex = 0u;
		section.sectionId = 0u;
		section.meshletOffset = 0u;
		section.meshletCount = 1u;
		section.primitiveOffset = 0u;
		section.primitiveCount = 1u;

		VirtualGeometryCullableGroupRecord group{};
		group.groupIndex = 0u;
		group.lodIndex = 0u;
		group.vertexOffset = 0u;
		group.vertexCount = 3u;
		group.primitiveOffset = 0u;
		group.primitiveCount = 1u;
		group.sectionId = 0u;
		group.pageId = 0u;
		group.boundsRadius = 1.0f;

		VirtualGeometryPageRecord page{};
		page.pageId = 0u;
		page.firstLodIndex = 0u;
		page.lodCount = 1u;
		page.groupOffset = 0u;
		page.groupCount = 1u;
		page.fallbackPageId = 0u;
		page.clusterPageLinkOffset = 0u;
		page.clusterPageLinkCount = 1u;
		page.priority = 1000u;
		page.byteSize = 128u;

		VirtualGeometryClusterRecord cluster{};
		cluster.clusterId = 0u;
		cluster.parentClusterId = kVirtualGeometryInvalidIndex;
		cluster.firstChildClusterId = kVirtualGeometryInvalidIndex;
		cluster.childCount = 0u;
		cluster.lodIndex = 0u;
		cluster.groupOffset = 0u;
		cluster.groupCount = 1u;
		cluster.materialSectionId = 0u;
		cluster.pageId = 0u;
		cluster.fallbackClusterId = 0u;
		cluster.boundsRadius = 1.0f;
		if (corruption == AdaptiveFixtureCorruption::ClusterSelfParent)
		{
			cluster.parentClusterId = 0u;
		}
		if (corruption == AdaptiveFixtureCorruption::MissingFallbackCluster)
		{
			cluster.fallbackClusterId = kVirtualGeometryInvalidIndex;
		}
		if (corruption == AdaptiveFixtureCorruption::InvalidMaterialSection)
		{
			cluster.materialSectionId = 99u;
		}

		VirtualGeometryClusterPageLinkRecord clusterPageLink{};
		clusterPageLink.clusterId = 0u;
		clusterPageLink.pageId = 0u;
		clusterPageLink.firstGroup = 0u;
		clusterPageLink.groupCount = 1u;
		clusterPageLink.flags = 0x3u;

		VirtualGeometryConeSafetyRecord coneSafety{};
		coneSafety.clusterId = 0u;
		coneSafety.groupOffset = 0u;
		coneSafety.groupCount = 1u;
		coneSafety.materialSidedness = 0u;
		coneSafety.windingState = 0u;
		coneSafety.alphaRisk = 0u;
		coneSafety.thinShellRisk = 0u;
		coneSafety.flags = kVirtualGeometryConeSafetyFlagConeValid | kVirtualGeometryConeSafetyFlagEligible;

		const VirtualGeometryRequestedTargetRecord requestedTargets[2] = {
			{ 0u, static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Generated), 1u, 1u, 1.0f },
			{ 1u, static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Skipped), 1u, 1u, 0.5f }
		};

		VirtualGeometrySkippedDetailRecord skipped{};
		skipped.targetIndex = 1u;
		skipped.status = static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Skipped);
		skipped.reason = static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::TooFewTriangles);
		skipped.nearestGeneratedLodIndex = 0u;
		skipped.messageStringOffset = skippedMessageOffset;
		skipped.messageStringSize = static_cast<std::uint32_t>(std::string_view("target has too few triangles").size());
		skipped.candidateTriangleCount = 0u;
		skipped.candidateMeshletCount = 0u;
		skipped.requestedRatio = 0.5f;
		skipped.actualCandidateRatio = 0.0f;
		skipped.actualNearestRatio = 1.0f;

		VirtualGeometryValidationFlagRecord validation{};
		validation.flags = kVirtualGeometryValidationFlagAdaptiveTargetsPresent
			| kVirtualGeometryValidationFlagFallbackCoverageValidated
			| kVirtualGeometryValidationFlagClusterHierarchyValidated
			| kVirtualGeometryValidationFlagPageLinksValidated
			| kVirtualGeometryValidationFlagConeSafetyMetadataValidated;
		validation.messageStringOffset = validationMessageOffset;
		validation.messageStringSize = static_cast<std::uint32_t>(std::string_view("fallback coverage validated").size());

		std::uint32_t cursor = AlignForTest(header.headerSize, kMeshletVertexDataAlignmentBytes);
		header.meshletVertexDataOffset = cursor;
		cursor = AlignForTest(cursor + header.meshletVertexCount * header.layout.strideBytes, kMeshletIndexDataAlignmentBytes);
		header.meshletVertexRemapDataOffset = cursor;
		cursor = AlignForTest(cursor + header.meshletVertexRemapCount * sizeof(std::uint32_t), kMeshletIndexDataAlignmentBytes);
		header.meshletPrimitiveIndexDataOffset = cursor;
		cursor = AlignForTest(cursor + header.meshletPrimitiveIndexCount * sizeof(std::uint8_t), kMeshletIndexDataAlignmentBytes);
		header.meshletTableOffset = cursor;
		cursor = AlignForTest(cursor + sizeof(MeshletRecord), kMeshletIndexDataAlignmentBytes);
		header.meshletSectionOffset = cursor;
		cursor = AlignForTest(cursor + sizeof(MeshletSection), kMeshletIndexDataAlignmentBytes);
		header.meshletBoundsOffset = cursor;
		cursor = AlignForTest(cursor + sizeof(MeshletBounds), 4u);
		header.virtualGeometryHeaderOffset = cursor;

		VirtualGeometryHeader virtualHeader{};
		virtualHeader.payloadGeneration = kVirtualGeometryPayloadGeneration;
		virtualHeader.headerSize = static_cast<std::uint32_t>(sizeof(VirtualGeometryHeader));
		virtualHeader.detailLevelCount = 1u;
		virtualHeader.materialSectionCount = 1u;
		virtualHeader.sectionRangeCount = 1u;
		virtualHeader.cullableGroupCount = 1u;
		virtualHeader.pageCount = 1u;
		virtualHeader.requestedTargetCount = 2u;
		virtualHeader.skippedDetailCount = 1u;
		virtualHeader.clusterCount = 1u;
		virtualHeader.clusterPageLinkCount = 1u;
		virtualHeader.coneSafetyCount = 1u;
		virtualHeader.validationFlagCount = 1u;
		virtualHeader.flags = validation.flags;
		virtualHeader.sourceTriangleCount = 1u;
		virtualHeader.sourceVertexCount = 3u;

		cursor += static_cast<std::uint32_t>(sizeof(VirtualGeometryHeader));
		const auto assignTable = [&](std::uint32_t& offset, std::uint32_t count, std::size_t stride) {
			cursor = AlignForTest(cursor, 4u);
			offset = count == 0u ? 0u : cursor;
			cursor += static_cast<std::uint32_t>(static_cast<std::size_t>(count) * stride);
		};
		assignTable(virtualHeader.detailLevelTableOffset, 1u, sizeof(detail));
		assignTable(virtualHeader.materialSectionTableOffset, 1u, sizeof(material));
		assignTable(virtualHeader.sectionRangeTableOffset, 1u, sizeof(section));
		assignTable(virtualHeader.cullableGroupTableOffset, 1u, sizeof(group));
		assignTable(virtualHeader.pageTableOffset, 1u, sizeof(page));
		assignTable(virtualHeader.requestedTargetTableOffset, 2u, sizeof(VirtualGeometryRequestedTargetRecord));
		assignTable(virtualHeader.skippedDetailTableOffset, 1u, sizeof(skipped));
		assignTable(virtualHeader.clusterTableOffset, 1u, sizeof(cluster));
		assignTable(virtualHeader.clusterPageLinkTableOffset, 1u, sizeof(clusterPageLink));
		assignTable(virtualHeader.coneSafetyTableOffset, 1u, sizeof(coneSafety));
		assignTable(virtualHeader.validationFlagTableOffset, 1u, sizeof(validation));
		assignTable(virtualHeader.diagnosticStringOffset, static_cast<std::uint32_t>(strings.size()), sizeof(char));
		virtualHeader.diagnosticStringSize = static_cast<std::uint32_t>(strings.size());

		std::vector<std::uint8_t> bytes(cursor, 0u);
		WriteValue(bytes, 0u, header);
		const std::uint32_t remap[3] = { 0u, 1u, 2u };
		std::copy(reinterpret_cast<const std::uint8_t*>(remap), reinterpret_cast<const std::uint8_t*>(remap) + sizeof(remap), bytes.begin() + header.meshletVertexRemapDataOffset);
		const std::uint8_t primitives[3] = { 0u, 1u, 2u };
		std::copy(primitives, primitives + sizeof(primitives), bytes.begin() + header.meshletPrimitiveIndexDataOffset);
		MeshletRecord meshlet{};
		meshlet.vertexCount = 3u;
		meshlet.primitiveIndexCount = 3u;
		WriteValue(bytes, header.meshletTableOffset, meshlet);
		MeshletSection meshletSection{};
		meshletSection.meshletCount = 1u;
		WriteValue(bytes, header.meshletSectionOffset, meshletSection);
		MeshletBounds bounds{};
		bounds.radius = 1.0f;
		bounds.coneCutoff = 2.0f;
		WriteValue(bytes, header.meshletBoundsOffset, bounds);
		WriteValue(bytes, header.virtualGeometryHeaderOffset, virtualHeader);
		WriteValue(bytes, virtualHeader.detailLevelTableOffset, detail);
		WriteValue(bytes, virtualHeader.materialSectionTableOffset, material);
		WriteValue(bytes, virtualHeader.sectionRangeTableOffset, section);
		WriteValue(bytes, virtualHeader.cullableGroupTableOffset, group);
		WriteValue(bytes, virtualHeader.pageTableOffset, page);
		WriteValue(bytes, virtualHeader.requestedTargetTableOffset, requestedTargets[0]);
		WriteValue(bytes, virtualHeader.requestedTargetTableOffset + static_cast<std::uint32_t>(sizeof(VirtualGeometryRequestedTargetRecord)), requestedTargets[1]);
		WriteValue(bytes, virtualHeader.skippedDetailTableOffset, skipped);
		WriteValue(bytes, virtualHeader.clusterTableOffset, cluster);
		WriteValue(bytes, virtualHeader.clusterPageLinkTableOffset, clusterPageLink);
		WriteValue(bytes, virtualHeader.coneSafetyTableOffset, coneSafety);
		WriteValue(bytes, virtualHeader.validationFlagTableOffset, validation);
		std::copy(strings.begin(), strings.end(), bytes.begin() + virtualHeader.diagnosticStringOffset);

		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}
}

	void RegisterVirtualGeometryResourceTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Resource.TemfRejectsStaleAdvertisedPayload", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryResourceTests" / "StaleVirtualGeometry.temf";
			WriteStaleVirtualGeometryTemf(path);

			ResourceManager& manager = ResourceManager::Instance();
			manager.ClearCaches();
			const TerraEngine::UUID meshId = manager.LoadMesh(path);

			context.Expect(manager.GetMesh(meshId) == nullptr, "Loader should reject TEMF files that advertise virtual geometry without a validated detail table; rebuild guidance is emitted through ResourceManager diagnostics.");
		});

		registry.Add("VirtualGeometry.Resource.ReloadKeepsPreviousMeshWhenVirtualGeometryMetadataIsInvalid", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryResourceTests" / "AtomicReloadVirtualGeometry.temf";
			WriteStaleVirtualGeometryTemf(path, false);

			ResourceManager& manager = ResourceManager::Instance();
			manager.ClearCaches();
			const TerraEngine::UUID meshId = manager.LoadMesh(path);
			const Mesh* originalMesh = manager.GetMesh(meshId);
			context.Expect(originalMesh != nullptr, "Fixture setup should load the valid flat TEMF before testing reload atomicity.");
			context.Expect(!originalMesh->hasVirtualGeometry, "The baseline fixture should be a non-virtual flat mesh.");
			context.ExpectEqual(static_cast<std::size_t>(1u), originalMesh->meshletRecords.size(), "The baseline fixture should expose one meshlet.");

			WriteStaleVirtualGeometryTemf(path, true);
			manager.ReloadMesh(meshId);
			const Mesh* reloadedMesh = manager.GetMesh(meshId);

			context.Expect(reloadedMesh != nullptr, "Invalid virtual geometry metadata should not evict the previous usable mesh on reload.");
			context.Expect(!reloadedMesh->hasVirtualGeometry, "Rejected virtual geometry metadata should not partially appear on the cached mesh.");
			context.Expect(reloadedMesh->virtualGeometryDetailLevels.empty(), "Rejected virtual geometry tables should not leak into cached mesh state.");
			context.ExpectEqual(static_cast<std::size_t>(1u), reloadedMesh->meshletRecords.size(), "The previous flat mesh payload should remain usable after failed reload.");
		});

		registry.Add("VirtualGeometry.Resource.LoadsAdaptiveRequestedAndSkippedRecords", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryResourceTests" / "AdaptiveRecords.temf";
			WriteAdaptiveVirtualGeometryTemf(path);

			ResourceManager& manager = ResourceManager::Instance();
			manager.ClearCaches();
			const TerraEngine::UUID meshId = manager.LoadMesh(path);
			const Mesh* mesh = manager.GetMesh(meshId);

			context.Expect(mesh != nullptr, "Loader should accept a coherent adaptive virtual geometry payload.");
			context.Expect(mesh->hasVirtualGeometry, "Loaded adaptive fixture should advertise virtual geometry.");
			context.ExpectEqual(static_cast<std::size_t>(2u), mesh->virtualGeometryRequestedTargets.size(), "Requested target table should load atomically.");
			context.ExpectEqual(static_cast<std::size_t>(1u), mesh->virtualGeometrySkippedDetails.size(), "Skipped detail table should load atomically.");
			context.ExpectEqual(static_cast<std::size_t>(1u), mesh->virtualGeometryClusters.size(), "Cluster table should load atomically with page metadata.");
			context.ExpectEqual(static_cast<std::size_t>(1u), mesh->virtualGeometryClusterPageLinks.size(), "Cluster-page links should load atomically with page metadata.");
			context.ExpectEqual(static_cast<std::size_t>(1u), mesh->virtualGeometryValidationFlags.size(), "Validation flag table should load atomically.");
			context.Expect(mesh->virtualGeometrySkippedDetails.front().reason == static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::TooFewTriangles), "Skipped detail reason should survive load.");
		});

		registry.Add("VirtualGeometry.Resource.SharedCompiledMeshPathResolvesEveryRegisteredGuid", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryResourceTests" / "SharedPackagePayload.temf";
			WriteStaleVirtualGeometryTemf(path, false);

			ResourceManager& manager = ResourceManager::Instance();
			manager.ClearCaches();
			const TerraEngine::UUID firstMeshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID secondMeshId = TerraEngine::GenerateUUID();

			// Model package imports give each generated child mesh a stable GUID,
			// but several children can intentionally point at the same cooked TEMF.
			// The resource manager must keep every component-facing GUID usable even
			// though the path cache loads the shared payload through one canonical id.
			manager.RegisterAssetPath(firstMeshId, path, AssetType::Mesh);
			manager.RegisterAssetPath(secondMeshId, path, AssetType::Mesh);

			const Mesh* firstMesh = manager.GetMesh(firstMeshId);
			const Mesh* secondMesh = manager.GetMesh(secondMeshId);

			context.Expect(firstMesh != nullptr, "The first registered mesh GUID should resolve even after another GUID points at the same cooked TEMF.");
			context.Expect(secondMesh != nullptr, "The canonical path GUID should still resolve normally.");
			context.Expect(firstMesh == secondMesh, "Shared package children should reuse the same loaded CPU mesh payload instead of requiring duplicate loads.");

			manager.ClearCaches();
			std::filesystem::remove(path);
		});

		registry.Add("VirtualGeometry.Resource.RejectsCyclicClusterHierarchy", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryResourceTests" / "CyclicCluster.temf";
			WriteAdaptiveVirtualGeometryTemf(path, AdaptiveFixtureCorruption::ClusterSelfParent);

			ResourceManager& manager = ResourceManager::Instance();
			manager.ClearCaches();
			const TerraEngine::UUID meshId = manager.LoadMesh(path);

			context.Expect(manager.GetMesh(meshId) == nullptr, "Loader should reject a cluster that is its own parent so mixed-detail cuts cannot overlap recursively.");
		});

		registry.Add("VirtualGeometry.Resource.RejectsMissingClusterFallbackCoverage", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryResourceTests" / "MissingClusterFallback.temf";
			WriteAdaptiveVirtualGeometryTemf(path, AdaptiveFixtureCorruption::MissingFallbackCluster);

			ResourceManager& manager = ResourceManager::Instance();
			manager.ClearCaches();
			const TerraEngine::UUID meshId = manager.LoadMesh(path);

			context.Expect(manager.GetMesh(meshId) == nullptr, "Loader should reject clusters without a fallback cluster so missing pages cannot create visible holes.");
		});

		registry.Add("VirtualGeometry.Resource.RejectsInvalidClusterMaterialOwnership", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryResourceTests" / "InvalidClusterMaterial.temf";
			WriteAdaptiveVirtualGeometryTemf(path, AdaptiveFixtureCorruption::InvalidMaterialSection);

			ResourceManager& manager = ResourceManager::Instance();
			manager.ClearCaches();
			const TerraEngine::UUID meshId = manager.LoadMesh(path);

			context.Expect(manager.GetMesh(meshId) == nullptr, "Loader should reject clusters whose material section ownership does not exist.");
		});
	}
}
