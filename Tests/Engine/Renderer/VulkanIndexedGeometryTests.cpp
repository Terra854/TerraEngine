// VulkanIndexedGeometryTests.cpp - Pure indexed topology preparation contracts.
// Registration is intentionally separate from native rendering acceptance.
#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanIndexedGeometry.h"
#include "File Formats/TerraEngineMeshFile.h"

#include <array>
#include <cstdint>
#include <future>
#include <limits>
#include <string>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace Renderer::Vulkan;
		struct IndexedFixture
		{
			Mesh mesh;
			std::array<std::uint8_t, 64> vertices{};
			std::array<std::uint8_t, 24> remaps{};
			std::array<std::uint8_t, 6> primitives{0, 1, 2, 0, 2, 1};
			explicit IndexedFixture(bool narrow)
			{
				mesh.publicationGeneration = 19;
				auto& layout = mesh.runtimeLayout;
				layout.layout.strideBytes = kMeshletPackedVertexStrideBytes;
				layout.meshletVertexCount = 4; layout.meshletVertexRemapCount = 6;
				layout.meshletCount = 2; layout.meshletPrimitiveIndexCount = 6; layout.meshletSectionCount = 2;
				layout.meshletVertexRemapEncoding = static_cast<std::uint32_t>(narrow ? MeshletRemapEncoding::Uint16 : MeshletRemapEncoding::Uint32);
				const std::uint32_t stride = narrow ? 2U : 4U;
				for (std::uint32_t index = 0; index < 6; ++index)
				{
					const auto owner = index < 3 ? index : index - 2;
					remaps[index * stride] = static_cast<std::uint8_t>(owner);
					mesh.meshletVertexRemapDecoded.push_back(owner);
				}
				mesh.meshletVertexData = vertices;
				mesh.meshletVertexRemapData = std::span<const std::uint8_t>(remaps.data(), 6U * stride);
				mesh.meshletPrimitiveIndexData = primitives;
				mesh.meshletRecords = {{0, 3, 0, 3}, {3, 3, 3, 3}};
				// Sections may select a later meshlet before a complete range. Their
				// authored order cannot renumber the shader's stable primitive ID.
				mesh.meshletSections = {{1, 1}, {0, 2}};
			}
		};
	}

	void RegisterVulkanIndexedGeometryTests(TestRegistry& registry)
	{
		registry.Add("Renderer.IndexedGeometry.CornerTokenDomainAndStableIdentity", [](TestContext& test) {
			std::uint32_t token = 99;
			test.Expect(TryEncodeIndexedCornerToken(2, 3, 2, token) && token == ((2U * 124U + 3U) * 3U + 2U), "The CPU token must match the vertex shader ABI.");
			const auto max = std::numeric_limits<std::uint32_t>::max();
			const auto primitiveIdentity = max / 3U;
			test.Expect(TryEncodeIndexedCornerToken(primitiveIdentity / 124U, primitiveIdentity % 124U, max % 3U, token) && token == max, "The final representable corner is valid.");
			for (const auto args : {std::array<std::uint32_t, 3>{0, 124, 0}, {0, 0, 3}, {max, 0, 0}, {primitiveIdentity / 124U, primitiveIdentity % 124U, 1}})
			{
				token = 99;
				test.Expect(!TryEncodeIndexedCornerToken(args[0], args[1], args[2], token) && token == 99, "Invalid corners and overflow preserve the previous token.");
			}
		});
		registry.Add("Renderer.IndexedGeometry.U16U32AndExactSectionRanges", [](TestContext& test) {
			for (bool narrow : {false, true})
			{
				IndexedFixture fixture(narrow); IndexedCornerGeometry output; std::string reason = "previous error";
				test.Expect(TryBuildIndexedCornerGeometry(fixture.mesh, output, reason) && reason.empty(), "Both real remap encodings prepare.");
				test.Expect(output.cpuPublicationGeneration == 19 && output.firstIndexByMeshlet == std::vector<std::uint32_t>{0, 3, 6}, "Corner prefixes retain the selected meshlet ranges and source publication.");
				test.Expect(output.sections == std::vector<IndexedCornerSection>{{3, 3, 1, 1}, {0, 6, 0, 2}}, "Each authored section gets its exact raster draw range.");
				test.Expect(ResolveIndexedCornerRange(output, 1, 1) == IndexedCornerSection{3, 3, 1, 1}
					&& !ResolveIndexedCornerRange(output, 1, 0) && !ResolveIndexedCornerRange(output, 1, 2),
					"The submitted range follows the exact selected meshlets and rejects empty or overrun ranges.");
				fixture.mesh.meshletVertexRemapDecoded.clear();
				test.Expect(TryBuildIndexedCornerGeometry(fixture.mesh, output, reason), "Raw bytes remain authoritative when the optional decoded cache is absent.");
			}
		});
		registry.Add("Renderer.IndexedGeometry.MalformedTopologyPreservesPriorPublication", [](TestContext& test) {
			for (std::uint32_t mutation = 0; mutation < 16; ++mutation)
			{
				IndexedFixture fixture(true); IndexedCornerGeometry output; std::string reason;
				test.Expect(TryBuildIndexedCornerGeometry(fixture.mesh, output, reason), "The baseline must prepare before each independent rejection.");
				const auto prior = output;
				switch (mutation)
				{
				case 0: fixture.mesh.publicationGeneration = 0; break;
				case 1: fixture.mesh.runtimeLayout.layout.strideBytes = 12; break;
				case 2: fixture.mesh.runtimeLayout.meshletVertexCount = 5; break;
				case 3: fixture.mesh.runtimeLayout.meshletVertexRemapEncoding = 7; break;
				case 4: fixture.mesh.runtimeLayout.meshletPrimitiveIndexCount = 7; break;
				case 5: fixture.mesh.runtimeLayout.meshletCount = 3; break;
				case 6: fixture.mesh.runtimeLayout.meshletSectionCount = 3; break;
				case 7: fixture.mesh.meshletRecords[0].vertexCount = 65; break;
				case 8: fixture.mesh.meshletRecords[0].primitiveIndexCount = 4; break;
				case 9: fixture.mesh.meshletRecords[0].vertexOffset = std::numeric_limits<std::uint32_t>::max(); break;
				case 10: fixture.mesh.meshletRecords[0].primitiveIndexOffset = std::numeric_limits<std::uint32_t>::max(); break;
				case 11: fixture.primitives[0] = 3; break;
				case 12: fixture.remaps[0] = 4; break;
				case 13: fixture.mesh.meshletVertexRemapDecoded[0] = 1; break;
				case 14: fixture.mesh.meshletSections[0].meshletOffset = std::numeric_limits<std::uint32_t>::max(); break;
				case 15: fixture.mesh.meshletVertexRemapData = fixture.mesh.meshletVertexRemapData.first(11); break;
				}
				test.Expect(!TryBuildIndexedCornerGeometry(fixture.mesh, output, reason) && !reason.empty() && output == prior, "Malformed source must reject atomically with the prior owned topology unchanged.");
			}
		});
		registry.Add("Renderer.IndexedGeometry.WideOwnerRemapUsesAllBytes", [](TestContext& test) {
			IndexedFixture fixture(false);
			std::vector<std::uint8_t> vertices(65537U * kMeshletPackedVertexStrideBytes);
			fixture.mesh.runtimeLayout.meshletVertexCount = 65537;
			fixture.mesh.meshletVertexData = vertices;
			fixture.remaps[0] = 0; fixture.remaps[1] = 0; fixture.remaps[2] = 1; fixture.remaps[3] = 0;
			fixture.mesh.meshletVertexRemapDecoded[0] = 65536;
			IndexedCornerGeometry output; std::string reason;
			test.Expect(TryBuildIndexedCornerGeometry(fixture.mesh, output, reason), "UINT32 remaps above UINT16 must retain their full owner address.");
		});
		registry.Add("Renderer.IndexedGeometry.ConcurrentImmutablePreparation", [](TestContext& test) {
			IndexedFixture fixture(true); IndexedCornerGeometry expected; std::string reason;
			test.Expect(TryBuildIndexedCornerGeometry(fixture.mesh, expected, reason), "A complete immutable source establishes the expected topology.");
			// Each job owns its result. The accepted Mesh and its raw payload spans
			// are shared read-only, matching renderer worker preparation ownership.
			std::array<std::future<bool>, 8> jobs;
			for (auto& job : jobs) job = std::async(std::launch::async, [&fixture, &expected] {
				for (std::uint32_t repeat = 0; repeat < 64; ++repeat)
				{
					IndexedCornerGeometry actual; std::string detail;
					if (!TryBuildIndexedCornerGeometry(fixture.mesh, actual, detail) || actual != expected) return false;
				}
				return true;
			});
			for (auto& job : jobs) test.Expect(job.get(), "Worker-local preparation must not race or mutate the shared source.");
		});
	}
}
