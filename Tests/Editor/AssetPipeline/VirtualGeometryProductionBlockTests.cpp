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
	}
} // namespace TerraEngine::EditorTests
