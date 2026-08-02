#include "TestHarness.h"

#include "VirtualGeometryProductionBlocks.h"
#include "Resource/VirtualGeometryCodec.h"

#include <array>
#include <vector>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor;

	void RegisterVirtualGeometryProductionBlockTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.AssetPipeline.ProductionBlocksAreDeterministicAndPageBounded",
			[](TestContext& context)
			{
				// A tiny metadata stream keeps this test independent from source-model
				// import while still exercising canonical directory and page assembly.
				const std::array<std::uint8_t, 8u> source = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
				TemfQuantizationDomain domain{};
				domain.domainVersion = 1u;
				domain.domainScope = static_cast<std::uint16_t>(TemfQuantizationDomainScope::Asset);
				domain.streamKind = static_cast<std::uint16_t>(TemfStreamKind::Metadata);
				domain.componentCount = 1u;
				domain.domainHash = 1u;
				const TemfProductionStreamInput stream{
					TemfStreamKind::Metadata, TemfCodecProfile::ExpandedProduction,
					0u, 0u, static_cast<std::uint32_t>(source.size()), 1u, source };
				const TemfProductionBuildRequest request{
					kTemfRepresentationVirtualGeometry, kTemfPageSize32KiB,
					17u, 19u, 23u, 29u, 31u, 0u,
					std::span<const TemfQuantizationDomain>(&domain, 1u),
					std::span<const TemfProductionStreamInput>(&stream, 1u) };
				const TemfProductionBuildResult first = BuildTemfProductionBlocks(request);
				const TemfProductionBuildResult second = BuildTemfProductionBlocks(request);
				context.Expect(first.IsComplete() && second.IsComplete() && first.bytes == second.bytes,
					"Canonical TEMF block assembly must be byte-identical for equal source and settings.");
				const auto validated = TerraEngine::Resource::ValidateTemfProductionFile(first.bytes, {});
				context.Expect(validated.IsComplete() && validated.directory.pages.size() == 1u
					&& validated.directory.blocks.size() == 1u,
					"Canonical paged output must reopen through the loader-grade production validator.");

				TemfProductionBuildRequest directRequest = request;
				directRequest.representationFlags = kTemfRepresentationDirectMeshlets;
				directRequest.logicalPageSizeBytes = 0u;
				const TemfProductionBuildResult direct = BuildTemfProductionBlocks(directRequest);
				const auto directValidated = TerraEngine::Resource::ValidateTemfProductionFile(direct.bytes, {});
				context.Expect(direct.IsComplete() && directValidated.IsComplete()
					&& directValidated.directory.header.pageCount == 0u
					&& directValidated.directory.header.pageTableOffset == 0u,
					"Direct-resident production output must publish no logical page table.");
			});

		registry.Add("VirtualGeometry.Editor.AssetPipeline.MultipleTopologyBlocksPublishOneDependencyRange",
			[](TestContext& context)
			{
				// Two topology blocks on one page reproduce the production import shape
				// used by dense deformable meshes. Both depend on the stable remap block,
				// so the page directory must retain the first dependency offset.
				const std::array<std::uint32_t, 3u> remap = { 0u, 1u, 2u };
				const std::array<std::uint32_t, 3u> indices = { 0u, 1u, 2u };
				std::array<TemfQuantizationDomain, 2u> domains{};
				domains[0].domainId = 0u;
				domains[0].domainVersion = 1u;
				domains[0].domainScope = static_cast<std::uint16_t>(TemfQuantizationDomainScope::Asset);
				domains[0].streamKind = static_cast<std::uint16_t>(TemfStreamKind::VertexRemapStableIdentity);
				domains[0].componentCount = 1u;
				domains[0].domainHash = 101u;
				domains[1].domainId = 1u;
				domains[1].domainVersion = 1u;
				domains[1].domainScope = static_cast<std::uint16_t>(TemfQuantizationDomainScope::Asset);
				domains[1].streamKind = static_cast<std::uint16_t>(TemfStreamKind::Topology);
				domains[1].componentCount = 1u;
				domains[1].domainHash = 103u;
				const auto remapBytes = std::as_bytes(std::span(remap));
				const auto indexBytes = std::as_bytes(std::span(indices));
				const std::array<TemfProductionStreamInput, 3u> streams = {
					TemfProductionStreamInput{ TemfStreamKind::VertexRemapStableIdentity,
						TemfCodecProfile::ExpandedProduction, 0u, 0u,
						static_cast<std::uint32_t>(remap.size()), sizeof(std::uint32_t),
						{ reinterpret_cast<const std::uint8_t*>(remapBytes.data()), remapBytes.size() } },
					TemfProductionStreamInput{ TemfStreamKind::Topology,
						TemfCodecProfile::MeshoptimizerProductionStatic, 1u, 0u,
						static_cast<std::uint32_t>(indices.size()), sizeof(std::uint32_t),
						{ reinterpret_cast<const std::uint8_t*>(indexBytes.data()), indexBytes.size() } },
					TemfProductionStreamInput{ TemfStreamKind::Topology,
						TemfCodecProfile::MeshoptimizerProductionStatic, 1u, 0u,
						static_cast<std::uint32_t>(indices.size()), sizeof(std::uint32_t),
						{ reinterpret_cast<const std::uint8_t*>(indexBytes.data()), indexBytes.size() } }
				};
				const TemfProductionBuildRequest request{
					kTemfRepresentationVirtualGeometry, kTemfPageSize32KiB,
					107u, 109u, 113u, 127u, 131u, 0u, domains, streams };
				const TemfProductionBuildResult result = BuildTemfProductionBlocks(request);
				const auto validated = TerraEngine::Resource::ValidateTemfProductionFile(result.bytes, {});
				context.Expect(result.IsComplete() && validated.IsComplete()
					&& validated.directory.pages.size() == 1u
					&& validated.directory.pages.front().firstDependencyIndex == 0u
					&& validated.directory.pages.front().dependencyCount == 2u,
					"A page with multiple topology blocks must publish one in-range dependency slice beginning at its first block.");
			});
	}
} // namespace TerraEngine::EditorTests
