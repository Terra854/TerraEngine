// TerrainDirectMeshTests.cpp -- Renderable terrain proxy mesh contract tests.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/Terrain/TerrainDirectMesh.h"
#include "Renderer/VirtualGeometry/Geometry/VirtualGeometryPackedVertex.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterTerrainDirectMeshTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Rendering.DirectMeshBuildsCanonicalVisiblePayload", [](TestContext& context) {
			std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount, 0);
			for(std::uint32_t z = 0u; z < 256u; ++z)
				for(std::uint32_t x = 0u; x < 256u; ++x)
					heights[static_cast<std::size_t>(z) * 256u + x] = static_cast<std::int32_t>(x * 10u + z * 5u);
			std::vector<std::uint8_t> holes(TerraEngine::Terrain::kTerrainAreaSampleCount / 8u, 0u);
			const std::uint32_t removedCell = 8u * 256u + 8u;
			holes[removedCell / 8u] |= static_cast<std::uint8_t>(1u << (removedCell % 8u));
			TerraEngine::Renderer::Terrain::TerrainDirectMeshInput input{};
			input.assetId = TerraEngine::GenerateUUID();
			input.authoredHash = {17u, 29u};
			input.authoredGeneration = 4u;
			input.sampleSpacingMetres = 2.0F;
			input.heightMillimetres = heights;
			input.holeCellBits = holes;
			auto result = TerraEngine::Renderer::Terrain::BuildTerrainDirectMesh(input);
			context.Expect(result.Succeeded(), "A complete authored terrain area should produce a direct-resident render mesh.");
			if(!result.Succeeded()) return;
			Mesh& mesh = *result.mesh;
			context.Expect(mesh.runtimeLayout.layout.strideBytes == kMeshletPackedVertexStrideBytes
				&& mesh.runtimeLayout.meshletVertexCount == TerraEngine::Terrain::kTerrainAreaSampleCount
				&& mesh.runtimeLayout.meshletCount == mesh.meshletRecords.size()
				&& mesh.meshletBounds.size() == mesh.meshletRecords.size(),
				"The generated terrain must use the renderer's canonical packed meshlet ABI with one conservative bound per meshlet.");
			const bool boundedMeshlets = std::all_of(mesh.meshletRecords.begin(), mesh.meshletRecords.end(), [](const MeshletRecord& record) {
				return record.vertexCount <= kMeshletMaxVertices
					&& record.primitiveIndexCount / 3u <= kMeshletMaxTriangles;
			});
			context.Expect(boundedMeshlets, "Every direct terrain meshlet must remain within the hardware mesh-shader vertex and triangle limits.");
			context.Expect(mesh.meshletPrimitiveIndexData.size() == 255u * 255u * 6u - 6u,
				"A painted hole must remove exactly its two triangles from the canonical primitive stream.");
			const std::uint64_t expectedResidentBytes =
				static_cast<std::uint64_t>(mesh.reconstructedVertexData.size())
				+ static_cast<std::uint64_t>(mesh.storage.size())
				+ static_cast<std::uint64_t>(mesh.meshletRecords.size() * sizeof(MeshletRecord))
				+ static_cast<std::uint64_t>(mesh.meshletSections.size() * sizeof(MeshletSection))
				+ static_cast<std::uint64_t>(mesh.meshletBounds.size() * sizeof(MeshletBounds));
			context.Expect(mesh.generatedDirectMeshletRoute.IsComplete()
				&& mesh.generatedDirectMeshletRoute.generationIdentity == input.authoredGeneration
				&& mesh.generatedDirectMeshletRoute.residentPayloadBytes == expectedResidentBytes,
				"A generated terrain mesh must publish typed direct-route evidence and remain on the generation-safe mesh-only pipeline.");
			context.Expect(RenderSystem::IsDirectMeshletRouteEligible(mesh),
				"The frame router must admit the complete generated terrain payload to the direct meshlet route.");
			++mesh.runtimeLayout.meshletVertexRemapCount;
			context.Expect(!RenderSystem::IsDirectMeshletRouteEligible(mesh),
				"Direct-route admission must fail closed when a generated terrain stream count no longer matches its immutable payload.");
			--mesh.runtimeLayout.meshletVertexRemapCount;
			input.sampleOriginMetres = std::array<double, 2u>{-512.0, -256.0};
			input.sampleStepMetres = {4.0F, 2.0F};
			auto overview = TerraEngine::Renderer::Terrain::BuildTerrainDirectMesh(input);
			context.Expect(overview.Succeeded() && overview.mesh->localBoundsMin[0] == -512.0F
				&& overview.mesh->localBoundsMin[2] == -256.0F
				&& overview.mesh->localBoundsMax[0] == 508.0F
				&& overview.mesh->localBoundsMax[2] == 254.0F,
				"A bounded overview proxy must cover its explicit authored origin and independent X/Z sample steps.");
			TerraEngine::VirtualGeometryPackedVertex first{};
			std::memcpy(&first, mesh.meshletVertexData.data(), sizeof(first));
			const auto decoded = TerraEngine::DecodeVirtualGeometryPackedPosition(first);
			const float decodedY = mesh.runtimeLayout.quantization.positionOffset[1]
				+ decoded[1] * mesh.runtimeLayout.quantization.positionScale[1];
			context.Expect(std::abs(decodedY) < 0.01F && mesh.hasLocalBounds
				&& mesh.localBoundsMax[0] == 510.0F && mesh.localBoundsMax[2] == 510.0F,
				"Packed terrain vertices and object bounds must decode back into the authored metre domain.");
		});

		registry.Add("Terrain.Rendering.DirectMeshRejectsFullyHoledArea", [](TestContext& context) {
			std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount, 0);
			std::vector<std::uint8_t> holes(TerraEngine::Terrain::kTerrainAreaSampleCount / 8u, 0xffu);
			TerraEngine::Renderer::Terrain::TerrainDirectMeshInput input{};
			input.assetId = TerraEngine::GenerateUUID();
			input.authoredHash = {31u, 47u};
			input.authoredGeneration = 1u;
			input.heightMillimetres = heights;
			input.holeCellBits = holes;
			const auto result = TerraEngine::Renderer::Terrain::BuildTerrainDirectMesh(input);
			context.Expect(!result.Succeeded() && !result.diagnostic.empty(),
				"A completely holed area must publish no empty or invalid meshlet payload.");
		});
	}
}
