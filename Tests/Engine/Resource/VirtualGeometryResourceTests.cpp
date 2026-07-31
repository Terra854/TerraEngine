#include "TestHarness.h"

#include "Editor/AssetCompilers/VirtualGeometryProductionBlocks.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/ResourceManager.h"
#include "Resource/VirtualGeometryCodec.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Editor;

	namespace
	{
		std::vector<std::uint8_t> BuildDirectProductionMesh(bool invalidLocalIndex = false)
		{
			// The fixture is deliberately small, but exercises the exact block set
			// that a renderer needs after canonical reconstruction.
			const std::array<std::uint8_t, 6> positions{};
			const std::array<std::uint8_t, 6> normalTangents{};
			const std::array<std::uint8_t, 4> textureColors{};
			const std::array<std::uint32_t, 1> remap = { 0u };
			const std::array<std::uint8_t, 3> topology = { invalidLocalIndex ? 1u : 0u, 0u, 0u };
			MeshletRecord record{};
			record.vertexCount = 1u;
			record.primitiveIndexCount = 3u;
			MeshletSection section{};
			section.meshletCount = 1u;
			MeshletBounds bounds{};
			bounds.radius = 1.0f;
			bounds.coneCutoff = 2.0f;
			TemfRuntimeDecodeMetadata runtime{};
			runtime.formatVersion = kMeshCurrentVersion;
			runtime.meshletVertexCount = 1u;
			runtime.meshletVertexRemapCount = 1u;
			runtime.meshletPrimitiveIndexCount = 3u;
			runtime.meshletCount = 1u;
			runtime.meshletSectionCount = 1u;
			runtime.meshletBoundsCount = 1u;
			runtime.meshletVertexRemapEncoding = static_cast<std::uint32_t>(MeshletRemapEncoding::Uint32);
			runtime.layout.strideBytes = 16u;
			runtime.quantization.positionScale[0] = 1.0f;

		TemfQuantizationDomain domain{};
		domain.domainId = 0u;
		domain.domainVersion = 1u;
		domain.domainScope = static_cast<std::uint16_t>(TemfQuantizationDomainScope::Asset);
		domain.streamKind = static_cast<std::uint16_t>(TemfStreamKind::Metadata);
		domain.componentCount = 1u;
		domain.componentBitWidths[0] = 32u;
		domain.extent[0] = 1.0;
		domain.quantizationStep[0] = 1.0;
		domain.domainHash = 1u;

		const auto stream = [](TemfStreamKind kind, std::uint64_t attributeMask, std::uint32_t count,
			std::uint32_t stride, std::span<const std::uint8_t> bytes) {
			TemfProductionStreamInput input{};
			input.streamKind = kind;
			input.codecProfile = TemfCodecProfile::ExpandedProduction;
			input.attributeMask = attributeMask;
			input.elementCount = count;
			input.elementStride = stride;
			input.decodedBytes = bytes;
			return input;
		};
		const std::array<TemfProductionStreamInput, 9> streams = {
			stream(TemfStreamKind::Position, 0u, 1u, 6u, positions),
			stream(TemfStreamKind::NormalTangent, 0u, 1u, 6u, normalTangents),
			stream(TemfStreamKind::TextureColor, 0u, 1u, 4u, textureColors),
			stream(TemfStreamKind::VertexRemapStableIdentity, 0u, 1u, sizeof(std::uint32_t), { reinterpret_cast<const std::uint8_t*>(remap.data()), sizeof(remap) }),
			stream(TemfStreamKind::Topology, 0u, 3u, 1u, topology),
			stream(TemfStreamKind::Metadata, kTemfMetadataRuntimeLayout, 1u, sizeof(runtime), { reinterpret_cast<const std::uint8_t*>(&runtime), sizeof(runtime) }),
			stream(TemfStreamKind::Metadata, kTemfMetadataMeshletRecords, 1u, sizeof(record), { reinterpret_cast<const std::uint8_t*>(&record), sizeof(record) }),
			stream(TemfStreamKind::Metadata, kTemfMetadataMeshletSections, 1u, sizeof(section), { reinterpret_cast<const std::uint8_t*>(&section), sizeof(section) }),
			stream(TemfStreamKind::Metadata, kTemfMetadataMeshletBounds, 1u, sizeof(bounds), { reinterpret_cast<const std::uint8_t*>(&bounds), sizeof(bounds) })
		};
		TemfProductionBuildRequest request{};
		request.representationFlags = kTemfRepresentationDirectMeshlets;
		request.logicalPageSizeBytes = 0u;
		request.quantizationDomains = std::span(&domain, 1u);
		request.streams = streams;
		const TemfProductionBuildResult result = BuildTemfProductionBlocks(request);
		return result.bytes;
		}

		void WriteBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
		{
			std::filesystem::create_directories(path.parent_path());
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
	}

	void RegisterVirtualGeometryResourceTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Resource.RejectsNonCanonicalTemf", [](TestContext& context) {
			const std::array<std::uint8_t, 8> staleBytes{};
			const auto validation = Resource::ValidateTemfProductionFile(staleBytes, {});
			context.Expect(!validation.IsComplete(), "A historical or truncated TEMF payload must require a canonical recook.");
		});

		registry.Add("VirtualGeometry.Resource.ReconstructsDirectProductionMesh", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryResourceTests" / "DirectProduction.temf";
			const std::vector<std::uint8_t> bytes = BuildDirectProductionMesh();
			context.Expect(!bytes.empty(), "The canonical direct-production fixture should assemble successfully.");
			WriteBytes(path, bytes);
			ResourceManager& manager = ResourceManager::Instance();
			manager.ClearCaches();
			const UUID meshId = manager.LoadMesh(path);
			const Mesh* mesh = manager.GetMesh(meshId);
			context.Expect(mesh != nullptr, "ResourceManager should accept and reconstruct a validated direct TEMFPRD1 mesh.");
			if (mesh != nullptr)
			{
				context.ExpectEqual(1u, mesh->runtimeLayout.meshletCount, "Runtime decode metadata should reconstruct meshlet counts.");
				context.ExpectEqual(static_cast<std::size_t>(16u), mesh->meshletVertexData.size(), "The position block should be upload-ready after decoding.");
				context.ExpectEqual(static_cast<std::size_t>(1u), mesh->meshletRecords.size(), "Meshlet record blocks should reconstruct typed runtime vectors.");
			}
			manager.ClearCaches();
			std::filesystem::remove(path);
		});

		registry.Add("VirtualGeometry.Resource.RejectsInvalidReconstructedLocalIndex", [](TestContext& context) {
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryResourceTests" / "InvalidLocalIndex.temf";
			WriteBytes(path, BuildDirectProductionMesh(true));
			ResourceManager& manager = ResourceManager::Instance();
			manager.ClearCaches();
			const UUID meshId = manager.LoadMesh(path);
			context.Expect(manager.GetMesh(meshId) == nullptr, "A decoded topology block with an out-of-range meshlet-local index must fail atomically.");
			manager.ClearCaches();
			std::filesystem::remove(path);
		});
	}
}
