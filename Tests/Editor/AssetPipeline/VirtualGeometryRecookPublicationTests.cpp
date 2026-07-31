#include "TestHarness.h"

#include "VirtualGeometryProductionBlocks.h"

#include <array>

namespace TerraEngine::EditorTests
{
	using namespace TerraEngine::Editor;

	void RegisterVirtualGeometryRecookPublicationTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.AssetPipeline.ProductionRecookRejectsInvalidStagingWithoutReplacingPriorBytes",
			[](TestContext& context)
			{
				// The production block writer is the worker-safe staging step. Keeping
				// a completed byte vector outside a failed request models the atomic
				// publisher contract: invalid source never overwrites known-good output.
				const std::array<std::uint8_t, 4u> source = { 3u, 5u, 7u, 11u };
				TemfQuantizationDomain domain{};
				domain.domainVersion = 1u;
				domain.domainScope = static_cast<std::uint16_t>(TemfQuantizationDomainScope::Asset);
				domain.streamKind = static_cast<std::uint16_t>(TemfStreamKind::Metadata);
				domain.componentCount = 1u;
				domain.domainHash = 1u;
				const TemfProductionStreamInput validStream{
					TemfStreamKind::Metadata, TemfCodecProfile::ExpandedProduction,
					0u, 0u, static_cast<std::uint32_t>(source.size()), 1u, source };
				TemfProductionBuildRequest request{};
				request.quantizationDomains = std::span<const TemfQuantizationDomain>(&domain, 1u);
				request.streams = std::span<const TemfProductionStreamInput>(&validStream, 1u);
				const TemfProductionBuildResult published = BuildTemfProductionBlocks(request);

				TemfProductionStreamInput invalidStream = validStream;
				invalidStream.elementCount = 0u;
				request.streams = std::span<const TemfProductionStreamInput>(&invalidStream, 1u);
				const TemfProductionBuildResult rejected = BuildTemfProductionBlocks(request);
				context.Expect(published.IsComplete() && !rejected.IsComplete() && !published.bytes.empty(),
					"Invalid source staging must reject before an atomic publisher replaces valid cooked output.");
			});
	}
} // namespace TerraEngine::EditorTests
