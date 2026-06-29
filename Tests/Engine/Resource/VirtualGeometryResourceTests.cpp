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
	}
}
